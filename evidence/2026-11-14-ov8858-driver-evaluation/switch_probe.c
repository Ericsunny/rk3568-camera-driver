/* Bounded six-frame V4L2 stream lifecycle. Does not detect void stop callbacks. */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_BUFS 16
static int xi(int fd, unsigned long request, void *arg) {
    int rc; do { rc = ioctl(fd, request, arg); } while (rc < 0 && errno == EINTR); return rc;
}
static double ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
int main(void) {
    void *maps[MAX_BUFS] = {0}; size_t lengths[MAX_BUFS] = {0};
    struct v4l2_format fmt; struct v4l2_requestbuffers req;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int fd = -1, on = 0, result = 1, saved = 0, i, count = 0;
    const char *stage = "open"; double start = 0, first = 0;
    memset(&req, 0, sizeof(req));
    fd = open("/dev/video14", O_RDWR | O_NONBLOCK);
    if (fd < 0) goto done;
    memset(&fmt, 0, sizeof(fmt)); fmt.type = type;
    fmt.fmt.pix_mp.width = 1280; fmt.fmt.pix_mp.height = 960;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    stage = "S_FMT";
    if (xi(fd, VIDIOC_S_FMT, &fmt) < 0) goto done;
    if (fmt.fmt.pix_mp.width != 1280 || fmt.fmt.pix_mp.height != 960 ||
        fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 || fmt.fmt.pix_mp.num_planes != 1) {
        errno = EINVAL; stage = "negotiated_format"; goto done;
    }
    req.count = 4; req.type = type; req.memory = V4L2_MEMORY_MMAP;
    stage = "REQBUFS";
    if (xi(fd, VIDIOC_REQBUFS, &req) < 0) goto done;
    if (!req.count || req.count > MAX_BUFS) { errno = EINVAL; goto done; }
    for (i = 0; i < (int)req.count; ++i) {
        struct v4l2_buffer b; struct v4l2_plane p;
        memset(&b, 0, sizeof(b)); memset(&p, 0, sizeof(p));
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.length = 1; b.m.planes = &p;
        stage = "QUERYBUF"; if (xi(fd, VIDIOC_QUERYBUF, &b) < 0) goto done;
        lengths[i] = p.length;
        stage = "mmap"; maps[i] = mmap(NULL, p.length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, p.m.mem_offset);
        if (maps[i] == MAP_FAILED) { maps[i] = NULL; goto done; }
        stage = "initial_QBUF"; if (xi(fd, VIDIOC_QBUF, &b) < 0) goto done;
    }
    stage = "STREAMON"; start = ms();
    if (xi(fd, VIDIOC_STREAMON, &type) < 0) goto done;
    on = 1;
    while (count < 6) {
        struct pollfd pollfd = {fd, POLLIN, 0};
        struct v4l2_buffer b; struct v4l2_plane p;
        int ready;
        stage = "poll";
        do { ready = poll(&pollfd, 1, 5000); } while (ready < 0 && errno == EINTR);
        if (ready == 0) { errno = ETIMEDOUT; goto done; }
        if (ready < 0 || (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))) goto done;
        memset(&b, 0, sizeof(b)); memset(&p, 0, sizeof(p));
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.length = 1; b.m.planes = &p;
        stage = "DQBUF"; if (xi(fd, VIDIOC_DQBUF, &b) < 0) goto done;
        if (b.index >= req.count || (b.flags & V4L2_BUF_FLAG_ERROR)) { errno = EIO; goto done; }
        if (!count) first = ms() - start;
        ++count;
        stage = "requeue_QBUF"; if (xi(fd, VIDIOC_QBUF, &b) < 0) goto done;
    }
    stage = "STREAMOFF"; if (xi(fd, VIDIOC_STREAMOFF, &type) < 0) goto done;
    on = 0; result = 0;
done:
    saved = errno;
    if (on && xi(fd, VIDIOC_STREAMOFF, &type) < 0) {
        fprintf(stderr, "cleanup STREAMOFF errno=%d\n", errno); result = 1;
    }
    for (i = 0; i < MAX_BUFS; ++i)
        if (maps[i] && munmap(maps[i], lengths[i]) < 0) result = 1;
    if (fd >= 0) {
        if (req.count) {
            memset(&req, 0, sizeof(req)); req.type = type; req.memory = V4L2_MEMORY_MMAP;
            if (xi(fd, VIDIOC_REQBUFS, &req) < 0) result = 1;
        }
        if (close(fd) < 0) result = 1;
    }
    if (result) fprintf(stderr, "RESULT fail stage=%s errno=%d frames=%d\n", stage, saved, count);
    else printf("RESULT ok frames=%d first_ms=%.2f\n", count, first);
    return result;
}
