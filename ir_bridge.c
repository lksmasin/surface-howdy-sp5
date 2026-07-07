#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <poll.h>

#define IN_WIDTH 640
#define IN_HEIGHT 480
#define IN_BYTES_PER_LINE 832
#define OUT_WIDTH 480
#define OUT_HEIGHT 640

// IPU3 ip3y format: 25 pixels in 32 bytes
void unpack_and_rotate_ipu3_line(const uint8_t *in, uint8_t *out_frame, int width, int y_in) {
    int out_idx = 0;
    int in_idx = 0;
    while (out_idx < width) {
        for (int i = 0; i < 6 && out_idx < width; i++) {
            if (out_idx < width) { out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] = (in[in_idx + 0] >> 2) | ((in[in_idx + 1] & 0x03) << 6); out_idx++; }
            if (out_idx < width) { out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] = (in[in_idx + 1] >> 4) | ((in[in_idx + 2] & 0x0F) << 4); out_idx++; }
            if (out_idx < width) { out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] = (in[in_idx + 2] >> 6) | ((in[in_idx + 3] & 0x3F) << 2); out_idx++; }
            if (out_idx < width) { out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] = (in[in_idx + 3] >> 8) | ((in[in_idx + 4] & 0xFF) << 0); out_idx++; }
            in_idx += 5;
        }
        if (out_idx < width) {
            out_frame[(IN_WIDTH - 1 - out_idx) * OUT_WIDTH + y_in] = (in[in_idx + 0] >> 2) | ((in[in_idx + 1] & 0x03) << 6); out_idx++;
        }
        in_idx += 2;
    }
}

int main(int argc, char **argv) {
    int fd_in = open("/dev/video12", O_RDWR);
    if (fd_in < 0) { perror("open /dev/video12"); return 1; }

    int fd_out = open("/dev/video42", O_RDWR);
    if (fd_out < 0) { perror("open /dev/video42"); return 1; }

    // Setup input format
    struct v4l2_format fmt_in = {0};
    fmt_in.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt_in.fmt.pix_mp.width = IN_WIDTH;
    fmt_in.fmt.pix_mp.height = IN_HEIGHT;
    fmt_in.fmt.pix_mp.pixelformat = v4l2_fourcc('i', 'p', '3', 'y');
    if (ioctl(fd_in, VIDIOC_S_FMT, &fmt_in) < 0) { perror("S_FMT in"); return 1; }

    // Setup output format
    struct v4l2_format fmt_out = {0};
    fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt_out.fmt.pix.width = OUT_WIDTH;
    fmt_out.fmt.pix.height = OUT_HEIGHT;
    fmt_out.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
    fmt_out.fmt.pix.sizeimage = OUT_WIDTH * OUT_HEIGHT;
    if (ioctl(fd_out, VIDIOC_S_FMT, &fmt_out) < 0) { perror("S_FMT out"); return 1; }

    // Request buffers for input
    struct v4l2_requestbuffers req = {0};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_in, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }

    struct { void *start; size_t length; } buffers[4];
    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[1] = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;
        if (ioctl(fd_in, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return 1; }
        buffers[i].length = buf.m.planes[0].length;
        buffers[i].start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_in, buf.m.planes[0].m.mem_offset);
        if (buffers[i].start == MAP_FAILED) { perror("mmap"); return 1; }
        if (ioctl(fd_in, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
    }

    // Start streaming
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_in, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }


    uint8_t *out_frame = malloc(OUT_WIDTH * OUT_HEIGHT);

    int led_triggered = 0;
    printf("Streaming started...\n");
    
    struct pollfd fds[1];
    fds[0].fd = fd_in;
    fds[0].events = POLLIN;

    while (1) {
        int r = poll(fds, 1, 5000);
        if (r == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (r == 0) continue; // Timeout

        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[1] = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;
        if (ioctl(fd_in, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF"); break;
        }

        if (led_triggered == 0) {
            // Enable the STROBE output on the OV7251 sensor so the IR LED lights up
            // Using absolute path so it survives the restricted PAM environment
            // Crucially: set real UID to 0, otherwise /bin/sh drops euid back to user during PAM!
            setuid(0);
            system("/usr/bin/i2ctransfer -f -y 3 w3@0x60 0x30 0x05 0x08");
            led_triggered = 1;
        }

        uint8_t *in_data = (uint8_t *)buffers[buf.index].start;
        for (int y = 0; y < IN_HEIGHT; y++) {
            unpack_and_rotate_ipu3_line(in_data + y * IN_BYTES_PER_LINE, out_frame, IN_WIDTH, y);
        }

        write(fd_out, out_frame, OUT_WIDTH * OUT_HEIGHT);

        if (ioctl(fd_in, VIDIOC_QBUF, &buf) < 0) { perror("QBUF loop"); break; }
    }

    return 0;
}
