/*
 * ir_bridge.c — Surface Pro 5 IR Camera Bridge for Howdy
 *
 * Reads raw 10-bit IPU3 (ip3y) frames from the OV7251 IR sensor,
 * unpacks to 8-bit GREY with simultaneous 90° CCW rotation,
 * and writes to a v4l2loopback device for Howdy consumption.
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
#define WARMUP_FRAMES      3       /* discard after LED trigger */
#define MIN_BRIGHTNESS     3       /* drop only dead-black frames     */
#define MAX_LIFETIME_SEC   15      /* hard self-timeout */
#define POLL_TIMEOUT_MS    2000
#define MAX_IDLE_POLLS     3       /* 3 × 2 s = 6 s idle → exit */
#define NUM_BUFFERS        4

/* ── Runtime state file (written by setup_ipu3.sh at boot) ───────── */
#define RUNTIME_DEV_FILE   "/run/surface_ir_bridge_dev"

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ── IPU3 ip3y unpack + 90° CCW rotation (fused, zero-copy) ─────── */
static void unpack_and_rotate_ipu3_line(const uint8_t *in,
                                        uint8_t *out_frame,
                                        int width, int y_in)
{
    int out_idx = 0;
    int in_idx  = 0;
    while (out_idx < width) {
        for (int i = 0; i < 6 && out_idx < width; i++) {
            if (out_idx < width) {
                out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] =
                    (in[in_idx + 0] >> 2) | ((in[in_idx + 1] & 0x03) << 6);
                out_idx++;
            }
            if (out_idx < width) {
                out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] =
                    (in[in_idx + 1] >> 4) | ((in[in_idx + 2] & 0x0F) << 4);
                out_idx++;
            }
            if (out_idx < width) {
                out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] =
                    (in[in_idx + 2] >> 6) | ((in[in_idx + 3] & 0x3F) << 2);
                out_idx++;
            }
            if (out_idx < width) {
                out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] =
                    (in[in_idx + 3] >> 8) | ((in[in_idx + 4] & 0xFF) << 0);
                out_idx++;
            }
            in_idx += 5;
        }
        if (out_idx < width) {
            out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] =
                (in[in_idx + 0] >> 2) | ((in[in_idx + 1] & 0x03) << 6);
            out_idx++;
        }
        in_idx += 2;
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
    /* Priority 1: explicit command-line argument */
    if (argc > 1)
        return argv[1];

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

    int led_ok          = 0;   /* 1 once LED is confirmed on           */
    int frames_after_led = 0;  /* frames dequeued since LED trigger     */
    int total_frames     = 0;
    int idle_polls       = 0;  /* consecutive poll timeouts             */

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
            for (int try = 0; try < LED_MAX_RETRIES; try++) {
                if (trigger_ir_led() == 0) {
                    led_ok = 1;
                    fprintf(stderr, "ir_bridge: LED on (attempt %d)\n",
                            try + 1);
                    break;
                }
                fprintf(stderr, "ir_bridge: LED attempt %d/%d failed\n",
                        try + 1, LED_MAX_RETRIES);
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

        /* Skip warmup frames so Howdy only gets post-LED-on frames */
        if (!led_ok || frames_after_led <= WARMUP_FRAMES) {
            ioctl(fd_in, VIDIOC_QBUF, &buf);
            continue;
        }

        /* Unpack + rotate */
        uint8_t *in_data = (uint8_t *)buffers[buf.index].start;
        for (int y = 0; y < IN_HEIGHT; y++)
            unpack_and_rotate_ipu3_line(in_data + y * IN_BYTES_PER_LINE,
                                        out_frame, IN_WIDTH, y);

        /* Brightness gate — drop dark frames instead of confusing Howdy */
        int bright = compute_mean_brightness(out_frame, OUT_WIDTH * OUT_HEIGHT);
        if (bright < MIN_BRIGHTNESS) {
            fprintf(stderr, "ir_bridge: dark frame dropped "
                    "(brightness=%d < %d)\n", bright, MIN_BRIGHTNESS);
            ioctl(fd_in, VIDIOC_QBUF, &buf);
            continue;
        }

        /* Write to loopback */
        ssize_t wr = write(fd_out, out_frame, OUT_WIDTH * OUT_HEIGHT);
        if (wr < 0)
            perror("ir_bridge: write loopback");

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

    fprintf(stderr, "ir_bridge: exit (%d frames processed)\n", total_frames);
    return 0;
}
