/*
 * ystat.c — 抓一帧 NV12,统计 Y 平面亮度(mean / max / p99),用于扫曝光/增益
 *
 * 编译:gcc -O2 -o ystat ystat.c
 * 用法:./ystat [device] [width] [height] [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define NBUF 4

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}

static int cmp(const void *a, const void *b)
{
	return (*(unsigned char *)a) - (*(unsigned char *)b);
}

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/video5";
	int w = (argc > 2) ? atoi(argv[2]) : 3840;
	int h = (argc > 3) ? atoi(argv[3]) : 2160;
	int nframes = (argc > 4) ? atoi(argv[4]) : 3;

	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }

	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
	fmt.fmt.pix_mp.width = w; fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	w = fmt.fmt.pix_mp.width; h = fmt.fmt.pix_mp.height;
	unsigned nplanes = fmt.fmt.pix_mp.num_planes;

	struct v4l2_requestbuffers req; memset(&req, 0, sizeof req);
	req.count = NBUF; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }

	void *mem[NBUF] = {0}; size_t len[NBUF] = {0};
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = nplanes;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
		len[i] = p[0].length;
		mem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, p[0].m.mem_offset);
		if (mem[i] == MAP_FAILED) { perror("mmap"); return 1; }
	}
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = nplanes;
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
	}
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

	size_t ysz = (size_t)w * h;
	unsigned char *sample = malloc(ysz);
	for (int k = 0; k < nframes; k++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.m.planes = p; b.length = nplanes;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); return 1; }
		const unsigned char *Y = (const unsigned char *)mem[b.index];
		memcpy(sample, Y, ysz);

		unsigned long long sum = 0; unsigned char mx = 0;
		unsigned long long hist[256] = {0};
		for (size_t i = 0; i < ysz; i++) {
			sum += sample[i]; hist[sample[i]]++; if (sample[i] > mx) mx = sample[i];
		}
		/* 用直方图求分位数:原来对 800 万像素 qsort 每帧要几百 ms,把计时全污染了 */
		unsigned long long acc = 0; unsigned char p50 = 0, p99 = 0;
		int got50 = 0, got99 = 0;
		for (int v = 0; v < 256; v++) {
			acc += hist[v];
			if (!got50 && acc >= ysz / 2)  { p50 = (unsigned char)v; got50 = 1; }
			if (!got99 && acc >= ysz * 99ULL / 100) { p99 = (unsigned char)v; got99 = 1; break; }
		}
		printf("frame %d: Y mean %.2f  p50 %3u  p99 %3u  max %3u  (size %dx%d)\n",
		       k, (double)sum / ysz, p50, p99, mx, w, h);
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF back"); return 1; }
	}

	xioctl(fd, VIDIOC_STREAMOFF, &type);
	for (unsigned i = 0; i < req.count; i++) munmap(mem[i], len[i]);
	close(fd); free(sample);
	return 0;
}
