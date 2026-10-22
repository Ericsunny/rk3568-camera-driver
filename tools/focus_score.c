/*
 * focus_score.c — 对焦辅助:连续采帧，逐帧算"对焦评分"，打印 + 记录峰值
 *
 * 为什么这么算：
 *   失焦 = 高频细节丢失 -> 图像梯度能量下降。把 Y 平面先做 4x4 盒式降采样
 *   再算 Sobel 梯度，既压掉 sensor 噪声(现在 NR 没开、噪点很重)，又保留
 *   真正的中频细节，评分随对焦变好单调上升。
 *
 * 编译: gcc -O2 -o focus_score focus_score.c
 * 用法: ./focus_score <dev> <帧数> [输出最佳帧文件] [宽 高]
 *   例: ./focus_score /dev/video5 300 /root/focus_best.nv12 3840 2160
 *
 * 输出: 每秒一行  "[  12.3s] score=<..> best=<..> Y=<..> |####--------|" ; 结束打印 MAX 并保存最佳帧
 *
 * 注意: 被 kill/Ctrl-C 时也会 STREAMOFF。否则 rkisp 会停在
 *       "rkisp_stream_stop id:0 timeout" 的坏状态, 之后任何程序都 DQBUF 不到帧。
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
#include <time.h>
#include <signal.h>

#define NBUF 4

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

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

/* 返回 Sobel 梯度均值(千分之一单位，整数便于比对) */
static long score_grad(const unsigned char *y, int w, int h, int stride, long *ylap, long *ymean)
{
	int dw = w / 4, dh = h / 4;
	static unsigned short *ds = NULL;
	static size_t cap = 0;
	size_t need = (size_t)dw * dh * sizeof(unsigned short);
	if (cap < need) { free(ds); ds = malloc(need); cap = need; }
	if (!ds) return -1;

	for (int j = 0; j < dh; j++) {
		const unsigned char *r0 = y + (size_t)(j * 4 + 1) * stride;
		const unsigned char *r1 = y + (size_t)(j * 4 + 2) * stride;
		unsigned short *o = ds + (size_t)j * dw;
		for (int i = 0; i < dw; i++) {
			const unsigned char *p0 = r0 + i * 4, *p1 = r1 + i * 4;
			unsigned s = p0[0] + p0[1] + p0[2] + p0[3] + p1[0] + p1[1] + p1[2] + p1[3];
			o[i] = (unsigned short)(s / 8);
		}
	}

	/* 只看中间 3/4 区域，避开暗角 */
	int x0 = dw / 8, x1 = dw - dw / 8, y0 = dh / 8, y1 = dh - dh / 8;
	if (x1 - x0 < 8 || y1 - y0 < 8) return -1;

	long long sum = 0, lsum = 0, ysum = 0;
	long n = 0;
	for (int j = y0; j < y1; j++) {
		const unsigned short *a = ds + (size_t)(j - 1) * dw;
		const unsigned short *b = ds + (size_t)j * dw;
		const unsigned short *c = ds + (size_t)(j + 1) * dw;
		for (int i = x0; i < x1; i++) {
			int gx = (int)c[i + 1] + 2 * (int)b[i + 1] + (int)a[i + 1]
			       - (int)c[i - 1] - 2 * (int)b[i - 1] - (int)a[i - 1];
			int gy = (int)a[i - 1] + 2 * (int)a[i] + (int)a[i + 1]
			       - (int)c[i - 1] - 2 * (int)c[i] - (int)c[i + 1];
			int g = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
			sum += g;
			int l = 4 * (int)b[i] - (int)a[i] - (int)c[i] - (int)b[i - 1] - (int)b[i + 1];
			lsum += (long long)l * l;
			ysum += b[i];
			n++;
		}
	}
	if (!n) return -1;
	*ylap = (long)(lsum / n);
	*ymean = (long)(ysum / n);
	return (long)(sum * 1000 / n);   /* 千分比，避免浮点 */
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		printf("usage: %s <dev> <frames> [best_out] [w h]\n", argv[0]);
		return 1;
	}
	const char *dev = argv[1];
	int want = atoi(argv[2]);
	const char *best_out = (argc > 3) ? argv[3] : NULL;
	int w = (argc > 4) ? atoi(argv[4]) : 3840;
	int h = (argc > 5) ? atoi(argv[5]) : 2160;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);

	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }
	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(fd, VIDIOC_G_FMT, &fmt);
	fmt.fmt.pix_mp.width = w; fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	w = fmt.fmt.pix_mp.width; h = fmt.fmt.pix_mp.height;
	unsigned np = fmt.fmt.pix_mp.num_planes;
	size_t sz = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	int stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (stride <= 0) stride = w;

	struct v4l2_requestbuffers req; memset(&req, 0, sizeof req);
	req.count = NBUF; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }

	void *mem[NBUF] = {0}; size_t len[NBUF] = {0};
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = np;
		xioctl(fd, VIDIOC_QUERYBUF, &b);
		len[i] = p[0].length;
		mem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, p[0].m.mem_offset);
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

	unsigned char *best = best_out ? malloc(sz) : NULL;
	long best_score = -1; int best_n = -1;
	double t0 = now_ms();

	for (int n = 0; n < want && !g_stop; n++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.m.planes = p; b.length = np;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }

		long lap = 0, ymean = 0;
		long g = score_grad((const unsigned char *)mem[b.index], w, h, stride, &lap, &ymean);
		if (g >= 0) {
			double t = now_ms() - t0;
			if (g > best_score) { best_score = g; best_n = n;
				if (best) memcpy(best, mem[b.index], sz); }
			if (n % 15 == 0) {
				long ref = best_score > 0 ? best_score : g;
				int k = (int)(g * 40 / (ref > 0 ? ref : 1));
				char bar[41];
				if (k > 40) k = 40;
				if (k < 0) k = 0;
				memset(bar, '-', 40); memset(bar, '#', k); bar[40] = 0;
				printf("[%6.1fs] score=%6ld lap=%7ld best=%6ld Y=%3ld |%s|\n",
				       t / 1000.0, g, lap, best_score, ymean, bar);
				fflush(stdout);
			}
		}
		xioctl(fd, VIDIOC_QBUF, &b);
		fflush(stdout);
	}
	xioctl(fd, VIDIOC_STREAMOFF, &bt);

	printf("\nMAX grad=%ld @frame %d\n", best_score, best_n);
	if (best && best_score > 0) {
		FILE *f = fopen(best_out, "wb");
		if (f) { fwrite(best, 1, sz, f); fclose(f); printf("最佳帧已存 -> %s (%zu 字节)\n", best_out, sz); }
	}
	free(best);
	for (unsigned i = 0; i < req.count; i++) munmap(mem[i], len[i]);
	close(fd);
	return 0;
}
