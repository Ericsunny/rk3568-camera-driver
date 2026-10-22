/*
 * st_burn — 板端出流稳定性压测工具
 *   -c N   开关流循环 N 次,每次记录首帧时延
 *   -t S   连续采集 S 秒,统计帧率与断流
 * 输出为机器可读的 CYCLE / LONG 行,由 st_regress.sh 汇总成报告
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define NBUFS 4

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct ctx {
	int fd;
	int w, h;
	unsigned int nbufs;
	struct v4l2_buffer bufs[NBUFS];
	struct v4l2_plane planes[NBUFS][1];
	void *mem[NBUFS];
	size_t len[NBUFS];
};

static int dev_open(struct ctx *c, const char *dev)
{
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	unsigned int i;

	c->fd = open(dev, O_RDWR);
	if (c->fd < 0)
		return -1;

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = c->w;
	f.fmt.pix_mp.height = c->h;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	if (ioctl(c->fd, VIDIOC_S_FMT, &f) < 0)
		goto err;

	memset(&rb, 0, sizeof(rb));
	rb.count = NBUFS;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0)
		goto err;
	c->nbufs = rb.count;

	for (i = 0; i < c->nbufs; i++) {
		struct v4l2_buffer *b = &c->bufs[i];

		memset(b, 0, sizeof(*b));
		memset(&c->planes[i][0], 0, sizeof(struct v4l2_plane));
		b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b->memory = V4L2_MEMORY_MMAP;
		b->index = i;
		b->length = 1;
		b->m.planes = &c->planes[i][0];
		if (ioctl(c->fd, VIDIOC_QUERYBUF, b) < 0)
			goto err;
		c->len[i] = b->m.planes[0].length;
		c->mem[i] = mmap(NULL, c->len[i], PROT_READ | PROT_WRITE,
				 MAP_SHARED, c->fd, b->m.planes[0].m.mem_offset);
		if (c->mem[i] == MAP_FAILED)
			goto err;
	}
	return 0;
err:
	close(c->fd);
	c->fd = -1;
	return -1;
}

static void dev_close(struct ctx *c)
{
	unsigned int i;

	if (c->fd < 0)
		return;
	for (i = 0; i < c->nbufs; i++)
		if (c->mem[i] && c->mem[i] != MAP_FAILED)
			munmap(c->mem[i], c->len[i]);
	close(c->fd);
	c->fd = -1;
}

static int qbuf(struct ctx *c, unsigned int i)
{
	struct v4l2_buffer *b = &c->bufs[i];

	memset(&c->planes[i][0], 0, sizeof(struct v4l2_plane));
	b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b->memory = V4L2_MEMORY_MMAP;
	b->index = i;
	b->length = 1;
	b->m.planes = &c->planes[i][0];
	return ioctl(c->fd, VIDIOC_QBUF, b);
}

static int dqbuf(struct ctx *c, int timeout_ms, unsigned int *idx)
{
	struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
	struct v4l2_buffer *b = &c->bufs[0];
	int r;

	r = poll(&pfd, 1, timeout_ms);
	if (r <= 0)
		return -1;

	memset(&c->planes[0][0], 0, sizeof(struct v4l2_plane));
	b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b->memory = V4L2_MEMORY_MMAP;
	b->length = 1;
	b->m.planes = &c->planes[0][0];
	if (ioctl(c->fd, VIDIOC_DQBUF, b) < 0)
		return -1;
	*idx = b->index;
	return 0;
}

/* 单次开流:返回首帧时延(ms),失败返回 -1 */
static double one_cycle(const char *dev, int w, int h)
{
	struct ctx c;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	unsigned int i, idx;
	double t0, dt;

	memset(&c, 0, sizeof(c));
	c.w = w;
	c.h = h;
	if (dev_open(&c, dev) < 0)
		goto fail;
	for (i = 0; i < c.nbufs; i++)
		if (qbuf(&c, i) < 0)
			goto fail;
	t0 = now_ms();
	if (ioctl(c.fd, VIDIOC_STREAMON, &type) < 0)
		goto fail;
	if (dqbuf(&c, 3000, &idx) < 0)
		goto fail_stream;
	dt = now_ms() - t0;
	qbuf(&c, idx);
	ioctl(c.fd, VIDIOC_STREAMOFF, &type);
	dev_close(&c);
	return dt;
fail_stream:
	ioctl(c.fd, VIDIOC_STREAMOFF, &type);
fail:
	dev_close(&c);
	return -1;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video5";
	int w = 1920, h = 1080, cycles = 0, secs = 0, opt, i, fails = 0;
	double *lat = NULL;
	int nlat = 0;

	while ((opt = getopt(argc, argv, "d:w:h:c:t:")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'w': w = atoi(optarg); break;
		case 'h': h = atoi(optarg); break;
		case 'c': cycles = atoi(optarg); break;
		case 't': secs = atoi(optarg); break;
		default: return 2;
		}
	}

	if (cycles > 0) {
		lat = calloc(cycles, sizeof(double));
		for (i = 0; i < cycles; i++) {
			double d = one_cycle(dev, w, h);

			if (d < 0) {
				fails++;
			} else {
				lat[nlat++] = d;
			}
			if ((i + 1) % 50 == 0)
				fprintf(stderr, "  [cycle %d/%d] fail=%d\n",
					i + 1, cycles, fails);
		}
		if (nlat > 0) {
			double sum = 0;
			qsort(lat, nlat, sizeof(double), cmp_d);
			for (i = 0; i < nlat; i++)
				sum += lat[i];
			printf("CYCLE cycles=%d ok=%d fail=%d fail_pct=%.2f "
			       "ff_min=%.1f ff_avg=%.1f ff_p50=%.1f ff_max=%.1f\n",
			       cycles, nlat, fails,
			       cycles ? fails * 100.0 / cycles : 0.0,
			       lat[0], sum / nlat, lat[nlat / 2], lat[nlat - 1]);
		} else {
			printf("CYCLE cycles=%d ok=0 fail=%d fail_pct=100.0 "
			       "ff_min=-1 ff_avg=-1 ff_p50=-1 ff_max=-1\n",
			       cycles, fails);
		}
		free(lat);
	}

	if (secs > 0) {
		struct ctx c;
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		double *iv = NULL, t0, t_end;
		long frames = 0, gaps = 0;
		unsigned int qi;
		int nint = 0, ret = 0;

		memset(&c, 0, sizeof(c));
		c.w = w;
		c.h = h;
		if (dev_open(&c, dev) < 0) {
			printf("LONG secs=%d frames=0 fps=0 gaps=0 loss_pct=100 "
			       "min_ms=-1 med_ms=-1 max_ms=-1\n", secs);
			return 1;
		}
		iv = calloc(200000, sizeof(double));
		for (qi = 0; qi < c.nbufs; qi++)
			qbuf(&c, qi);
		t0 = now_ms();
		if (ioctl(c.fd, VIDIOC_STREAMON, &type) < 0)
			ret = 1;
		t_end = now_ms();
		while (!ret) {
			unsigned int idx;
			double ts;

			if (dqbuf(&c, 2000, &idx) < 0) {
				gaps++;
				if (gaps > 5) {
					ret = 2;
					break;
				}
				continue;
			}
			ts = now_ms();
			if (ts - t0 > secs * 1000.0) {
				qbuf(&c, idx);
				break;
			}
			if (frames > 0 && nint < 199999 &&
			    ts >= t_end) {
				iv[nint++] = ts - t_end;
			} else if (ts < t_end) {
				/* 首帧 */
			}
			if (frames > 0)
				; /* interval 累积 */
			t_end = ts;
			frames++;
			qbuf(&c, idx);
		}
		ioctl(c.fd, VIDIOC_STREAMOFF, &type);
		dev_close(&c);

		{
			double el = (now_ms() - t0) / 1000.0;
			double mn = -1, md = -1, mx = -1;
			long over = 0;

			if (nint > 5) {
				double med;
				int k;

				qsort(iv, nint, sizeof(double), cmp_d);
				med = iv[nint / 2];
				mn = iv[0];
				mx = iv[nint - 1];
				md = med;
				/* 断流:间隔超过中位数的 2 倍 */
				for (k = 0; k < nint; k++)
					if (iv[k] > med * 2.0)
						over++;
			}
			printf("LONG secs=%d frames=%ld fps=%.2f gaps=%ld "
			       "loss_pct=%.3f min_ms=%.1f med_ms=%.1f max_ms=%.1f rc=%d\n",
			       secs, frames, el > 0 ? frames / el : 0.0,
			       gaps + over,
			       frames + gaps + over ?
			       (gaps + over) * 100.0 / (frames + gaps + over) : 0.0,
			       mn, md, mx, ret);
		}
		free(iv);
	}

	return fails ? 1 : 0;
}
