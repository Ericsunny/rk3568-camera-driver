/*
 * capbench.c — 采集基准:固定时长内数帧,报告 fps、每帧间隔、进程 CPU 时间
 *
 * 为什么不用 v4l2-ctl --stream-count:多路并行时它没法分别报各自的速率与 CPU。
 *
 * 编译:gcc -O2 -o capbench capbench.c
 * 用法:./capbench <device> <秒> [宽 高 fourcc]
 *   fourcc: NV12 / BG10(10bit bayer)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <linux/videodev2.h>
#include <time.h>

#define NBUF 4

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}
static double now_ms(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static unsigned int fourcc_of(const char *s)
{
	if (!strcmp(s, "NV12")) return V4L2_PIX_FMT_NV12;
	if (!strcmp(s, "BG10")) return V4L2_PIX_FMT_SBGGR10;
	return V4L2_PIX_FMT_NV12;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		printf("usage: %s <dev> <seconds> [w h fourcc]\n", argv[0]);
		return 1;
	}
	const char *dev = argv[1];
	double dur = atof(argv[2]) * 1000.0;
	int w = (argc > 3) ? atoi(argv[3]) : 3840;
	int h = (argc > 4) ? atoi(argv[4]) : 2160;
	unsigned int pf = (argc > 5) ? fourcc_of(argv[5]) : V4L2_PIX_FMT_NV12;

	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }

	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
	fmt.fmt.pix_mp.width = w; fmt.fmt.pix_mp.height = h; fmt.fmt.pix_mp.pixelformat = pf;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	w = fmt.fmt.pix_mp.width; h = fmt.fmt.pix_mp.height;
	unsigned np = fmt.fmt.pix_mp.num_planes;
	unsigned szimg = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	struct v4l2_requestbuffers req; memset(&req, 0, sizeof req);
	req.count = NBUF; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }

	void *mem[NBUF] = {0}; size_t len[NBUF] = {0};
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = np;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
		len[i] = p[0].length;
		mem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, p[0].m.mem_offset);
		if (mem[i] == MAP_FAILED) { perror("mmap"); return 1; }
	}
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = np;
		xioctl(fd, VIDIOC_QBUF, &b);
	}
	enum v4l2_buf_type bt = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &bt) < 0) { perror("STREAMON"); return 1; }

	struct rusage r0, r1;
	getrusage(RUSAGE_SELF, &r0);
	double t0 = now_ms();
	double t_start = t0, t_last = 0;
	int n = 0, warmup = 3;
	double first_ts = 0, last_ts = 0;

	while (now_ms() - t_start < dur) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.m.planes = p; b.length = np;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
		double ts = b.timestamp.tv_sec * 1000.0 + b.timestamp.tv_usec / 1000.0;
		if (n >= warmup) {
			if (first_ts == 0) first_ts = ts;
			last_ts = ts;
		}
		if (n >= warmup) t_last = now_ms();
		xioctl(fd, VIDIOC_QBUF, &b);
		n++;
	}
	double t1 = now_ms();
	getrusage(RUSAGE_SELF, &r1);
	xioctl(fd, VIDIOC_STREAMOFF, &bt);

	double cpu_ms = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) * 1000.0
	              + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1000.0
	              + (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1000.0
	              + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1000.0;
	int valid = n - warmup;
	double wall = t1 - t0;
	double span = (last_ts > first_ts) ? (last_ts - first_ts) : wall;
	double fps = (valid > 0 && span > 0) ? (valid - 1) * 1000.0 / span : 0;

	printf("%-13s %4dx%-4d %-4s  %3d 帧  %6.2f fps  %6.2f ms/帧  CPU %6.1f ms/帧(%4.1f%%)  每帧 %6.2f MB  -> %6.1f MB/s\n",
	       dev, w, h, pf == V4L2_PIX_FMT_NV12 ? "NV12" : "BG10",
	       valid, fps, fps > 0 ? 1000.0 / fps : 0,
	       valid ? cpu_ms / valid : 0, wall > 0 ? 100.0 * cpu_ms / wall : 0,
	       szimg / 1048576.0, szimg * fps / 1048576.0);

	for (unsigned i = 0; i < req.count; i++) munmap(mem[i], len[i]);
	close(fd);
	return 0;
}
