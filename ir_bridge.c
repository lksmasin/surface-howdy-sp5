/*
 * ir_bridge.c — Surface Pro 5 IR Camera Bridge for Howdy
 *
 * Reads raw 10-bit IPU3 (ip3y) frames from the OV7251 IR sensor,
 * unpacks to 8-bit GREY with simultaneous 90° CCW rotation,
 * and writes to a v4l2loopback device for Howdy consumption.
 *
 * IPU3 ip3y pixel format (V4L2_PIX_FMT_IPU3_Y10):
 *   25 pixels packed into 32 bytes as a contiguous little-endian
 *   10-bit bitstream. 6 groups of 4 pixels in 5 bytes (30 bytes),
 *   plus 1 pixel in 2 bytes (with 6 bits padding).
 *   Bit layout per 5-byte group:
 *     Byte 0: Y'0[7:0]
 *     Byte 1: Y'1[5:0] | Y'0[9:8]
 *     Byte 2: Y'2[3:0] | Y'1[9:6]
 *     Byte 3: Y'3[1:0] | Y'2[9:4]
 *     Byte 4: Y'3[9:2]
 *   To convert 10-bit to 8-bit: value >> 2
 *   Reference: kernel Documentation/userspace-api/media/v4l/pixfmt-yuv-luma.rst
 *
 * Security considerations:
 *   - No shell-out: I2C LED control is done via direct ioctl, not system()
 *   - No setuid(0): privilege manipulation is unnecessary with direct I2C
 *   - Device path validation: only /dev/videoN paths accepted
 *   - Self-timeout: process exits automatically after MAX_LIFETIME_SEC
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

/* ── Frame geometry ───────────────────────────────────────────────── */
#define IN_WIDTH           640
#define IN_HEIGHT          480
#define IN_BYTES_PER_LINE  832
#define OUT_WIDTH          480   /* rotated */
#define OUT_HEIGHT         640

/* ── I2C / LED settings ──────────────────────────────────────────── */
#define I2C_BUS            "/dev/i2c-3"
#define I2C_ADDR           0x60
#define LED_MAX_RETRIES    3
#define LED_RETRY_DELAY_US 50000   /* 50 ms */

/* ── Pipeline tuning ─────────────────────────────────────────────── */
#define WARMUP_FRAMES      5       /* discard after LED trigger          */
#define MIN_BRIGHTNESS     3       /* drop only dead-sensor frames       */
#define MAX_LIFETIME_SEC   15      /* hard self-timeout                  */
#define POLL_TIMEOUT_MS    2000
#define MAX_IDLE_POLLS     3       /* 3 × 2 s = 6 s idle → exit         */
#define NUM_BUFFERS        4

/* ── Runtime state file (written by setup_ipu3.sh at boot) ───────── */
#define RUNTIME_DEV_FILE   "/run/surface_ir_bridge_dev"

/* ── Debug frame dump ────────────────────────────────────────────── */
#define DEBUG_DUMP_DIR     "/tmp/ir_bridge_debug"
#define DEBUG_MAX_FRAMES   5

static volatile sig_atomic_t running = 1;
static int debug_mode = 0;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/*
 * ── IPU3 ip3y unpack + 90° CCW rotation ────────────────────────────
 *
 * Each scanline is packed as 25 pixels per 32 bytes:
 *   - 6 × (4 pixels in 5 bytes) = 24 pixels in 30 bytes
 *   - 1 pixel in 2 bytes (6 bits padding in last byte)
 *
 * For a group of 4 pixels in 5 bytes (b0..b4):
 *   Y'0 = b0 | (b1 & 0x03) << 8   — 10-bit, >> 2 → 8-bit
 *   Y'1 = (b1 >> 2) | (b2 & 0x0F) << 6
 *   Y'2 = (b2 >> 4) | (b3 & 0x3F) << 4
 *   Y'3 = (b3 >> 6) | b4 << 2      — top 8 bits = b4
 *
 * Rotation: physical sensor is mounted sideways, so we apply a 90° CCW
 * rotation during unpack. Input pixel (x, y) maps to output pixel
 * (y, IN_WIDTH-1-x) in the rotated OUT_WIDTH × OUT_HEIGHT frame.
 */
static void unpack_and_rotate_ipu3_line(const uint8_t *in,
                                        uint8_t *out_frame,
                                        int width, int y_in)
{
    int x = 0;
    int byte_idx = 0;

    /* Process 6 groups of 4 pixels (24 pixels, 30 bytes) */
    for (int group = 0; group < 6 && x < width; group++) {
        const uint8_t b0 = in[byte_idx + 0];
        const uint8_t b1 = in[byte_idx + 1];
        const uint8_t b2 = in[byte_idx + 2];
        const uint8_t b3 = in[byte_idx + 3];
        const uint8_t b4 = in[byte_idx + 4];

        /* Y'0: bits [9:0] = b0[7:0] | b1[1:0]<<8.   8-bit = >>2 */
        if (x < width) {
            uint8_t val = (b0 >> 2) | ((b1 & 0x03) << 6);
            out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] = val;
            x++;
        }
        /* Y'1: bits [9:0] = b1[7:2] | b2[3:0]<<6.   8-bit = >>2 */
        if (x < width) {
            uint8_t val = (b1 >> 4) | ((b2 & 0x0F) << 4);
            out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] = val;
            x++;
        }
        /* Y'2: bits [9:0] = b2[7:4] | b3[5:0]<<4.   8-bit = >>2 */
        if (x < width) {
            uint8_t val = (b2 >> 6) | ((b3 & 0x3F) << 2);
            out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] = val;
            x++;
        }
        /* Y'3: bits [9:0] = b3[7:6] | b4[7:0]<<2.   8-bit = b4 */
        if (x < width) {
            out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] = b4;
            x++;
        }
        byte_idx += 5;
    }

    /* 25th pixel in the 32-byte block (same formula as Y'0) */
    if (x < width) {
        uint8_t val = (in[byte_idx] >> 2) | ((in[byte_idx + 1] & 0x03) << 6);
        out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] = val;
        x++;
    }
    /* byte_idx += 2; → advances to byte 32, start of next block */

    /* If width > 25, continue with subsequent 32-byte blocks.
     * For 640px width: 640 / 25 = 25.6 → 26 blocks × 32 = 832 bytes/line
     * which matches IN_BYTES_PER_LINE. */
    byte_idx += 2;

    while (x < width) {
        /* Full groups of 4 within this block */
        int remaining = width - x;
        int groups = (remaining >= 24) ? 6 : (remaining / 4);

        for (int group = 0; group < groups; group++) {
            const uint8_t b0 = in[byte_idx + 0];
            const uint8_t b1 = in[byte_idx + 1];
            const uint8_t b2 = in[byte_idx + 2];
            const uint8_t b3 = in[byte_idx + 3];
            const uint8_t b4 = in[byte_idx + 4];

            if (x < width) {
                out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] =
                    (b0 >> 2) | ((b1 & 0x03) << 6);
                x++;
            }
            if (x < width) {
                out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] =
                    (b1 >> 4) | ((b2 & 0x0F) << 4);
                x++;
            }
            if (x < width) {
                out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] =
                    (b2 >> 6) | ((b3 & 0x3F) << 2);
                x++;
            }
            if (x < width) {
                out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] = b4;
                x++;
            }
            byte_idx += 5;
        }

        /* 25th pixel */
        if (x < width) {
            out_frame[(IN_WIDTH - 1 - x) * OUT_WIDTH + y_in] =
                (in[byte_idx] >> 2) | ((in[byte_idx + 1] & 0x03) << 6);
            x++;
        }
        byte_idx += 2;
    }
}

/* ── Direct I2C LED trigger (replaces system() + setuid(0)) ──────── */
static int trigger_ir_led(void)
{
    int fd = open(I2C_BUS, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ir_bridge: open %s: %s\n", I2C_BUS, strerror(errno));
        return -1;
    }

    /*
     * I2C_SLAVE_FORCE is required because the ov7251 kernel driver
     * already holds address 0x60. Regular I2C_SLAVE would refuse.
     */
    if (ioctl(fd, I2C_SLAVE_FORCE, I2C_ADDR) < 0) {
        fprintf(stderr, "ir_bridge: I2C_SLAVE_FORCE 0x%02x: %s\n",
                I2C_ADDR, strerror(errno));
        close(fd);
        return -1;
    }

    /* OV7251 register 0x3005 = 0x08  →  enable strobe output (IR LED) */
    uint8_t buf[3] = { 0x30, 0x05, 0x08 };
    ssize_t n = write(fd, buf, sizeof(buf));
    if (n != (ssize_t)sizeof(buf)) {
        fprintf(stderr, "ir_bridge: I2C write: %zd/%zu bytes: %s\n",
                n, sizeof(buf), (n < 0) ? strerror(errno) : "short write");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/* ── Brightness check (sampled, cheap) ───────────────────────────── */
static int compute_mean_brightness(const uint8_t *frame, int size)
{
    long sum   = 0;
    int  count = 0;
    for (int i = 0; i < size; i += 64) {
        sum += frame[i];
        count++;
    }
    return count > 0 ? (int)(sum / count) : 0;
}

/* ── Debug frame dump ────────────────────────────────────────────── */
static void dump_debug_frame(const uint8_t *frame, int size, int frame_num,
                             int brightness)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/frame_%04d_b%03d.raw",
             DEBUG_DUMP_DIR, frame_num, brightness);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, frame, size);
        close(fd);
        fprintf(stderr, "ir_bridge: debug dump → %s\n", path);
    }
}

/* ── Validate device path ────────────────────────────────────────── */
static int validate_video_device(const char *path)
{
    /* Must match /dev/videoN */
    if (strncmp(path, "/dev/video", 10) != 0) {
        fprintf(stderr, "ir_bridge: rejected path '%s': "
                "must start with /dev/video\n", path);
        return -1;
    }
    const char *p = path + 10;
    if (*p == '\0') {
        fprintf(stderr, "ir_bridge: rejected path '%s': "
                "no device number\n", path);
        return -1;
    }
    for (; *p; p++) {
        if (*p < '0' || *p > '9') {
            fprintf(stderr, "ir_bridge: rejected path '%s': "
                    "invalid char '%c'\n", path, *p);
            return -1;
        }
    }

    /* Confirm it's actually a V4L2 device */
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ir_bridge: open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "ir_bridge: '%s' not a V4L2 device: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* ── Resolve which /dev/videoN to use ────────────────────────────── */
static const char *resolve_device_path(int argc, char **argv)
{
    /* Priority 1: explicit command-line argument (skip flags) */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-')
            return argv[i];
    }

    /* Priority 2: runtime state file written by setup_ipu3.sh */
    static char path_buf[64];
    FILE *f = fopen(RUNTIME_DEV_FILE, "r");
    if (f) {
        if (fgets(path_buf, sizeof(path_buf), f)) {
            path_buf[strcspn(path_buf, "\n")] = '\0';
            fclose(f);
            return path_buf;
        }
        fclose(f);
    }

    /* Fallback */
    return "/dev/video2";
}

/* ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);

    /* Check for -d / --debug flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
            fprintf(stderr, "ir_bridge: debug mode enabled, "
                    "dumping frames to %s\n", DEBUG_DUMP_DIR);
        }
    }

    if (debug_mode) {
        /* Create debug dump directory */
        mkdir(DEBUG_DUMP_DIR, 0755);
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* ── Resolve & validate input device ─────────────────────────── */
    const char *dev_in = resolve_device_path(argc, argv);
    if (validate_video_device(dev_in) < 0)
        return 1;

    int fd_in = open(dev_in, O_RDWR);
    if (fd_in < 0) {
        fprintf(stderr, "ir_bridge: open %s: %s\n", dev_in, strerror(errno));
        return 1;
    }

    int fd_out = open("/dev/video42", O_RDWR);
    if (fd_out < 0) {
        perror("ir_bridge: open /dev/video42");
        return 1;
    }

    /* ── Input format (IPU3 ip3y, 10-bit packed) ─────────────────── */
    struct v4l2_format fmt_in = {0};
    fmt_in.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt_in.fmt.pix_mp.width    = IN_WIDTH;
    fmt_in.fmt.pix_mp.height   = IN_HEIGHT;
    fmt_in.fmt.pix_mp.pixelformat = v4l2_fourcc('i','p','3','y');
    if (ioctl(fd_in, VIDIOC_S_FMT, &fmt_in) < 0) {
        perror("ir_bridge: S_FMT in");
        return 1;
    }

    /* ── Output format (8-bit GREY, rotated) ─────────────────────── */
    struct v4l2_format fmt_out = {0};
    fmt_out.type               = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt_out.fmt.pix.width      = OUT_WIDTH;
    fmt_out.fmt.pix.height     = OUT_HEIGHT;
    fmt_out.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
    fmt_out.fmt.pix.sizeimage  = OUT_WIDTH * OUT_HEIGHT;
    if (ioctl(fd_out, VIDIOC_S_FMT, &fmt_out) < 0) {
        perror("ir_bridge: S_FMT out");
        return 1;
    }

    /* ── Request + mmap input buffers ────────────────────────────── */
    struct v4l2_requestbuffers req = {0};
    req.count  = NUM_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_in, VIDIOC_REQBUFS, &req) < 0) {
        perror("ir_bridge: REQBUFS");
        return 1;
    }

    struct { void *start; size_t length; } buffers[NUM_BUFFERS];
    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer buf   = {0};
        struct v4l2_plane  pl[1] = {0};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.planes = pl;
        buf.length   = 1;
        if (ioctl(fd_in, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("ir_bridge: QUERYBUF");
            return 1;
        }
        buffers[i].length = pl[0].length;
        buffers[i].start  = mmap(NULL, pl[0].length,
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd_in, pl[0].m.mem_offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("ir_bridge: mmap");
            return 1;
        }
        if (ioctl(fd_in, VIDIOC_QBUF, &buf) < 0) {
            perror("ir_bridge: QBUF init");
            return 1;
        }
    }

    /* ── Start streaming ─────────────────────────────────────────── */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_in, VIDIOC_STREAMON, &type) < 0) {
        perror("ir_bridge: STREAMON");
        return 1;
    }

    uint8_t *out_frame = malloc(OUT_WIDTH * OUT_HEIGHT);
    if (!out_frame) {
        perror("ir_bridge: malloc");
        return 1;
    }

    int led_ok           = 0;   /* 1 once LED is confirmed on          */
    int frames_after_led = 0;   /* frames dequeued since LED trigger   */
    int total_frames     = 0;
    int written_frames   = 0;   /* frames actually sent to loopback    */
    int dropped_dark     = 0;   /* frames dropped for being too dark   */
    int idle_polls       = 0;   /* consecutive poll timeouts            */
    int debug_dumped     = 0;   /* frames dumped in debug mode          */

    fprintf(stderr, "ir_bridge: started pid=%d dev=%s\n", getpid(), dev_in);

    struct pollfd pfd = { .fd = fd_in, .events = POLLIN };

    /* ── Main capture loop ───────────────────────────────────────── */
    while (running) {
        /* Hard self-timeout */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec >= MAX_LIFETIME_SEC) {
            fprintf(stderr, "ir_bridge: lifetime limit (%ds), exiting\n",
                    MAX_LIFETIME_SEC);
            break;
        }

        int r = poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("ir_bridge: poll");
            break;
        }
        if (r == 0) {
            if (++idle_polls >= MAX_IDLE_POLLS) {
                fprintf(stderr, "ir_bridge: idle timeout, exiting\n");
                break;
            }
            continue;
        }
        idle_polls = 0;

        /* Dequeue one frame */
        struct v4l2_buffer buf   = {0};
        struct v4l2_plane  pl[1] = {0};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.m.planes = pl;
        buf.length   = 1;
        if (ioctl(fd_in, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            perror("ir_bridge: DQBUF");
            break;
        }
        total_frames++;

        /*
         * Trigger LED after the first successful dequeue.
         *
         * We CANNOT trigger before VIDIOC_STREAMON because the ov7251
         * kernel driver re-initializes sensor registers during its
         * start-streaming path, which overwrites any prior I2C writes.
         * Waiting for the first DQBUF guarantees the driver's init is
         * complete and the I2C bus is free.
         */
        if (!led_ok && total_frames == 1) {
            for (int attempt = 0; attempt < LED_MAX_RETRIES; attempt++) {
                if (trigger_ir_led() == 0) {
                    led_ok = 1;
                    fprintf(stderr, "ir_bridge: LED on (attempt %d)\n",
                            attempt + 1);
                    break;
                }
                fprintf(stderr, "ir_bridge: LED attempt %d/%d failed\n",
                        attempt + 1, LED_MAX_RETRIES);
                usleep(LED_RETRY_DELAY_US);
            }
            if (!led_ok) {
                fprintf(stderr, "ir_bridge: WARNING: LED failed after "
                        "%d retries — frames will be dark\n",
                        LED_MAX_RETRIES);
            }
            frames_after_led = 0;
        }

        if (led_ok) frames_after_led++;

        /* Skip warmup frames so sensor/LED can stabilize */
        if (!led_ok || frames_after_led <= WARMUP_FRAMES) {
            ioctl(fd_in, VIDIOC_QBUF, &buf);
            continue;
        }

        /* Unpack + rotate */
        uint8_t *in_data = (uint8_t *)buffers[buf.index].start;
        for (int y = 0; y < IN_HEIGHT; y++)
            unpack_and_rotate_ipu3_line(in_data + y * IN_BYTES_PER_LINE,
                                        out_frame, IN_WIDTH, y);

        /* Brightness gate — filter out LED-off strobe frames.
         *
         * The OV7251 in strobe mode alternates LED on/off every frame.
         * LED-on frames have mean brightness ~25, LED-off frames ~5.
         * We only pass through LED-on (bright) frames to Howdy, which
         * eliminates the alternating-darkness pattern that confuses
         * face detection. */
        int bright = compute_mean_brightness(out_frame, OUT_WIDTH * OUT_HEIGHT);

        /* Debug: dump first N frames regardless of brightness */
        if (debug_mode && debug_dumped < DEBUG_MAX_FRAMES) {
            dump_debug_frame(out_frame, OUT_WIDTH * OUT_HEIGHT,
                             total_frames, bright);
            debug_dumped++;
        }

        if (bright < MIN_BRIGHTNESS) {
            dropped_dark++;
            if (dropped_dark <= 3 || dropped_dark % 50 == 0) {
                fprintf(stderr, "ir_bridge: dark frame dropped "
                        "(brightness=%d < %d, total_dropped=%d)\n",
                        bright, MIN_BRIGHTNESS, dropped_dark);
            }
            ioctl(fd_in, VIDIOC_QBUF, &buf);
            continue;
        }

        /* Write to loopback */
        ssize_t wr = write(fd_out, out_frame, OUT_WIDTH * OUT_HEIGHT);
        if (wr < 0)
            perror("ir_bridge: write loopback");
        else
            written_frames++;

        if (ioctl(fd_in, VIDIOC_QBUF, &buf) < 0) {
            perror("ir_bridge: QBUF loop");
            break;
        }
    }

    /* ── Cleanup ─────────────────────────────────────────────────── */
    int stop = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd_in, VIDIOC_STREAMOFF, &stop);
    for (int i = 0; i < (int)req.count; i++)
        munmap(buffers[i].start, buffers[i].length);
    free(out_frame);
    close(fd_in);
    close(fd_out);

    fprintf(stderr, "ir_bridge: exit (total=%d written=%d dropped_dark=%d)\n",
            total_frames, written_frames, dropped_dark);
    return 0;
}
