/*
 * reg_switch.c — 回归测试的单次开关流探针(带首帧计时)
 *
 * 一次完整生命周期:open→S_FMT→REQBUFS→mmap→QBUF×4→STREAMON→
 *   DQBUF(首帧,计时)→再收 n 帧(算 fps)→STREAMOFF→清理。
 * 输出一行机器可解析结果,shell 循环收集,python 汇总。
 *
 * 用法: reg_switch <dev> <w> <h> <fourcc> [extra_frames]
 *   fourcc: NV12 / BG10
 * 输出:
 *   RESULT ok dev=%s first_ms=%.1f frames=%d fps=%.1f
 *   RESULT fail dev=%s stage=%s errno=%d
 * 退出码:0=ok 1=fail
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define NBUF 4

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "用法:%s <dev> <w> <h> <fourcc> [extra_frames]\n", argv[0]);
        return 1;
    }
    const char *dev = argv[1];
    int w = atoi(argv[2]), h = atoi(argv[3]);
    unsigned int fourcc;
    if (!strcmp(argv[4], "NV12"))      fourcc = V4L2_PIX_FMT_NV12;
    else if (!strcmp(argv[4], "BG10")) fourcc = V4L2_PIX_FMT_SBGGR10;
    else { printf("RESULT fail dev=%s stage=fourcc errno=0\n", dev); return 1; }
    int extra = argc > 5 ? atoi(argv[5]) : 15;

    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("RESULT fail dev=%s stage=open errno=%d\n", dev, errno); return 1; }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = fourcc;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("RESULT fail dev=%s stage=s_fmt errno=%d\n", dev, errno); close(fd); return 1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NBUF; req.type = fmt.type; req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("RESULT fail dev=%s stage=reqbufs errno=%d\n", dev, errno); close(fd); return 1;
    }

    void *maps[NBUF]; size_t lens[NBUF];
    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = fmt.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = pl; b.length = 1;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("RESULT fail dev=%s stage=querybuf errno=%d\n", dev, errno); close(fd); return 1;
        }
        lens[i] = pl[0].length;
        maps[i] = mmap(NULL, pl[0].length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, pl[0].m.mem_offset);
        if (maps[i] == MAP_FAILED) {
            printf("RESULT fail dev=%s stage=mmap errno=%d\n", dev, errno); close(fd); return 1;
        }
    }
    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer b; struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = fmt.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = pl; b.length = 1;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            printf("RESULT fail dev=%s stage=qbuf errno=%d\n", dev, errno); close(fd); return 1;
        }
    }

    double t_on = now_ms();
    if (xioctl(fd, VIDIOC_STREAMON, &fmt.type) < 0) {
        printf("RESULT fail dev=%s stage=streamon errno=%d\n", dev, errno); close(fd); return 1;
    }

    /* 首帧(阻塞 DQBUF,60s 超时兜底) */
    struct v4l2_buffer b; struct v4l2_plane pl[1];
    memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
    b.type = fmt.type; b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = pl; b.length = 1;
    fd_set fds; struct timeval tv;
    FD_ZERO(&fds); FD_SET(fd, &fds); tv.tv_sec = 60; tv.tv_usec = 0;
    int sr = select(fd+1, &fds, NULL, NULL, &tv);
    if (sr <= 0) {
        printf("RESULT fail dev=%s stage=first_frame errno=%d\n", dev, sr == 0 ? ETIMEDOUT : errno);
        goto out_off;
    }
    if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
        printf("RESULT fail dev=%s stage=first_dqbuf errno=%d\n", dev, errno);
        goto out_off;
    }
    double t_first = now_ms();
    double first_ms = t_first - t_on;
    /* 首帧也回填队列 */
    {
        struct v4l2_buffer b2; struct v4l2_plane pl2[1];
        memset(&b2, 0, sizeof(b2)); memset(pl2, 0, sizeof(pl2));
        b2.type = fmt.type; b2.memory = V4L2_MEMORY_MMAP; b2.index = b.index;
        b2.m.planes = pl2; b2.length = 1;
        xioctl(fd, VIDIOC_QBUF, &b2);
    }

    /* extra 帧测 fps */
    int got = 0;
    double t_a = now_ms();
    while (got < extra) {
        FD_ZERO(&fds); FD_SET(fd, &fds); tv.tv_sec = 10; tv.tv_usec = 0;
        sr = select(fd+1, &fds, NULL, NULL, &tv);
        if (sr <= 0) break;
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = fmt.type; b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl; b.length = 1;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) break;
        got++;
        {
            struct v4l2_buffer b2; struct v4l2_plane pl2[1];
            memset(&b2, 0, sizeof(b2)); memset(pl2, 0, sizeof(pl2));
            b2.type = fmt.type; b2.memory = V4L2_MEMORY_MMAP; b2.index = b.index;
            b2.m.planes = pl2; b2.length = 1;
            xioctl(fd, VIDIOC_QBUF, &b2);
        }
    }
    double dt = now_ms() - t_a;
    double fps = got > 0 ? got * 1000.0 / dt : 0.0;

    xioctl(fd, VIDIOC_STREAMOFF, &fmt.type);
    for (int i = 0; i < (int)req.count; i++) munmap(maps[i], lens[i]);
    /* 释放全部 buffer,让下一次循环从干净状态开始(也验证 REQBUFS(0) 路径) */
    memset(&req, 0, sizeof(req));
    req.count = 0; req.type = fmt.type; req.memory = V4L2_MEMORY_MMAP;
    xioctl(fd, VIDIOC_REQBUFS, &req);
    close(fd);
    printf("RESULT ok dev=%s first_ms=%.1f frames=%d fps=%.1f\n", dev, first_ms, got, fps);
    return 0;

out_off:
    xioctl(fd, VIDIOC_STREAMOFF, &fmt.type);
    for (int i = 0; i < (int)req.count; i++) munmap(maps[i], lens[i]);
    memset(&req, 0, sizeof(req));
    req.count = 0; req.type = fmt.type; req.memory = V4L2_MEMORY_MMAP;
    xioctl(fd, VIDIOC_REQBUFS, &req);
    close(fd);
    return 1;
}
