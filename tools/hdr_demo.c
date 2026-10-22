/*
 * hdr_demo.c —— 静态场景 HDR 多帧融合演示（RK3568 / IMX415）
 *
 * 原理：同一场景用 3 档曝光各抓一帧，在线性域按"曝光适中度 × 局部对比度"
 *       加权融合，输出一帧 NV12。适用于静止场景，运动场景会有鬼影。
 *
 * 用法：
 *   hdr_demo -d /dev/video5 -s /dev/v4l-subdev6 -w 3840 -h 2160 \
 *            -e 60,300,1400 -n 6 -o /root/hdr/fused.nv12
 *
 * 注意：运行前必须停掉 rkaiq_3A，否则 AE 会立刻覆盖手动曝光。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>

#define MAXBUF 8
#define RADIUS 8          /* 权重平滑半径 */
#define SIGMA 0.22f

static int W = 3840, H = 2160;
static const char *g_video = "/dev/video5";
static int g_dev_fd = -1;
static unsigned char *g_buf[MAXBUF];
static size_t g_len[MAXBUF];
static int g_nbuf;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void die(const char *msg)
{
	fprintf(stderr, "ERR: %s: %s\n", msg, strerror(errno));
	exit(1);
}

/* ---------- 采集 ---------- */

static void cap_open(void)
{
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	int i;

	g_dev_fd = open(g_video, O_RDWR);
	if (g_dev_fd < 0)
		die("open video");

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = W;
	f.fmt.pix_mp.height = H;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	if (ioctl(g_dev_fd, VIDIOC_S_FMT, &f) < 0)
		die("VIDIOC_S_FMT");
	W = f.fmt.pix_mp.width;
	H = f.fmt.pix_mp.height;

	memset(&rb, 0, sizeof(rb));
	rb.count = 4;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(g_dev_fd, VIDIOC_REQBUFS, &rb) < 0)
		die("REQBUFS");
	g_nbuf = rb.count;

	for (i = 0; i < g_nbuf; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane p;
		memset(&b, 0, sizeof(b));
		memset(&p, 0, sizeof(p));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.m.planes = &p;
		b.length = 1;
		if (ioctl(g_dev_fd, VIDIOC_QUERYBUF, &b) < 0)
			die("QUERYBUF");
		g_len[i] = p.length;
		g_buf[i] = mmap(NULL, p.length, PROT_READ | PROT_WRITE,
				MAP_SHARED, g_dev_fd, p.m.mem_offset);
		if (g_buf[i] == MAP_FAILED)
			die("mmap");
		if (ioctl(g_dev_fd, VIDIOC_QBUF, &b) < 0)
			die("QBUF");
	}

	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(g_dev_fd, VIDIOC_STREAMON, &t) < 0)
		die("STREAMON");
}

static void cap_close(void)
{
	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(g_dev_fd, VIDIOC_STREAMOFF, &t);
	close(g_dev_fd);
}

/* 取一帧，把 Y 平面拷到 dst */
static int cap_grab(unsigned char *dst_y, unsigned char *dst_uv)
{
	struct v4l2_buffer b;
	struct v4l2_plane p;
	int ret;

	memset(&b, 0, sizeof(b));
	memset(&p, 0, sizeof(p));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &p;
	b.length = 1;
	ret = ioctl(g_dev_fd, VIDIOC_DQBUF, &b);
	if (ret < 0)
		return -1;
	size_t ysz = (size_t)W * H;
	if (dst_y)
		memcpy(dst_y, g_buf[b.index], ysz);
	if (dst_uv)
		memcpy(dst_uv, g_buf[b.index] + ysz, ysz / 2);
	ioctl(g_dev_fd, VIDIOC_QBUF, &b);
	return 0;
}

static void set_exposure(int sfd, int val)
{
	struct v4l2_control c;
	memset(&c, 0, sizeof(c));
	c.id = V4L2_CID_EXPOSURE;
	c.value = val;
	if (ioctl(sfd, VIDIOC_S_CTRL, &c) < 0)
		fprintf(stderr, "WARN: set exposure %d failed: %s\n", val, strerror(errno));
	usleep(300 * 1000);
}

/* ---------- 融合 ---------- */

static unsigned char g_gauss[256];
static float g_lin[256];
static float g_degamma[4097];

static void build_lut(void)
{
	int i;
	for (i = 0; i < 256; i++) {
		float n = i / 255.0f;
		float g = expf(-((n - 0.5f) * (n - 0.5f)) / (2 * SIGMA * SIGMA));
		g_gauss[i] = (unsigned char)(254.0f * g + 1.0f);
		g_lin[i] = powf(n, 2.2f);
	}
	for (i = 0; i <= 4096; i++) {
		float v = (float)i / 4096.0f;
		g_degamma[i] = powf(v, 1 / 2.2f) * 255.0f;
	}
}

/* 权重：gauss(亮度) × (1 + 0.5×局部对比度/64)   → uint16 */
static unsigned short *make_weight(const unsigned char *y)
{
	int x, yy;
	unsigned short *w = malloc((size_t)W * H * sizeof(unsigned short));
	if (!w)
		die("malloc weight");

	for (yy = 0; yy < H; yy++) {
		const unsigned char *row = y + (size_t)yy * W;
		const unsigned char *up = y + (size_t)(yy > 0 ? yy - 1 : 0) * W;
		const unsigned char *dn = y + (size_t)(yy < H - 1 ? yy + 1 : H - 1) * W;
		unsigned short *out = w + (size_t)yy * W;
		for (x = 0; x < W; x++) {
			int l = row[x > 0 ? x - 1 : 0];
			int r = row[x < W - 1 ? x + 1 : W - 1];
			int c = 4 * row[x] - up[x] - dn[x] - l - r;
			unsigned f;
			if (c < 0)
				c = -c;
			if (c > 255)
				c = 255;
			f = 256 + (unsigned)c;          /* 1 .. 1.5 倍 */
			out[x] = (unsigned short)((g_gauss[row[x]] * f) >> 8);
		}
	}
	return w;
}

/* 可分离盒式平滑：先横后竖，滑动窗口 */
static unsigned int *hpass(const unsigned short *w)
{
	unsigned int *t = malloc((size_t)W * H * sizeof(unsigned int));
	int x, y, k;
	if (!t)
		die("malloc hpass");
	for (y = 0; y < H; y++) {
		const unsigned short *row = w + (size_t)y * W;
		unsigned int *out = t + (size_t)y * W;
		unsigned int sum = 0;
		for (k = 0; k <= RADIUS && k < W; k++)
			sum += row[k];
		for (x = 0; x < W; x++) {
			out[x] = sum;
			if (x - RADIUS >= 0)
				sum -= row[x - RADIUS];
			if (x + RADIUS + 1 < W)
				sum += row[x + RADIUS + 1];
		}
	}
	return t;
}

int main(int argc, char **argv)
{
	const char *subdev = "/dev/v4l-subdev6", *outpath = NULL;
	int exposures[8], nexp = 0, nframes = 6, i, j, keep = 0;
	int sfd;
	double t0, t_cap, t_w, t_blur, t_fuse, t_all;
	unsigned char **ys, *uv_mid = NULL, *fused;
	unsigned short **ws;
	unsigned int **ts;
	unsigned int *colsum[8];

	while ((i = getopt(argc, argv, "d:s:w:h:e:n:o:k")) != -1) {
		switch (i) {
		case 'd': g_video = optarg; break;
		case 's': subdev = optarg; break;
		case 'w': W = atoi(optarg); break;
		case 'h': H = atoi(optarg); break;
		case 'n': nframes = atoi(optarg); break;
		case 'o': outpath = optarg; break;
		case 'k': keep = 1; break;
		case 'e': {
			char *p = optarg, *q;
			while (p && *p && nexp < 8) {
				exposures[nexp++] = atoi(p);
				q = strchr(p, ',');
				p = q ? q + 1 : NULL;
			}
			break;
		}
		default:
			fprintf(stderr, "usage: %s -d dev -s subdev -w W -h H -e 60,300,1400 -n N -o out.nv12 [-k 保留每档输入]\n", argv[0]);
			return 1;
		}
	}
	if (!nexp || !outpath) {
		fprintf(stderr, "需要 -e 和 -o\n");
		return 1;
	}

	printf(">>> %dx%d NV12, %d 档曝光, 每档丢弃前 %d 帧\n", W, H, nexp, nframes);
	build_lut();

	sfd = open(subdev, O_RDWR);
	if (sfd < 0)
		die("open subdev");

	ys = calloc(nexp, sizeof(*ys));
	for (i = 0; i < nexp; i++) {
		ys[i] = malloc((size_t)W * H);
		if (!ys[i])
			die("malloc y");
	}

	t_all = now_ms();
	t0 = now_ms();
	cap_open();
	/* 预热，丢掉前几帧 */
	for (i = 0; i < 6; i++)
		cap_grab(NULL, NULL);

	for (i = 0; i < nexp; i++) {
		set_exposure(sfd, exposures[i]);
		for (j = 0; j < nframes; j++) {
			if (cap_grab(ys[i], (i == nexp / 2 && !uv_mid) ? (uv_mid = malloc((size_t)W * H / 2)) : NULL) < 0) {
				fprintf(stderr, "DQBUF 失败\n");
				return 1;
			}
		}
		printf("    曝光 %5d 抓 %d 帧完成\n", exposures[i], nframes);
	}
	cap_close();
	close(sfd);
	t_cap = now_ms() - t0;

	t0 = now_ms();
	ws = calloc(nexp, sizeof(*ws));
	ts = calloc(nexp, sizeof(*ts));
	for (i = 0; i < nexp; i++)
		ws[i] = make_weight(ys[i]);
	t_w = now_ms() - t0;

	t0 = now_ms();
	for (i = 0; i < nexp; i++)
		ts[i] = hpass(ws[i]);
	t_blur = now_ms() - t0;

	t0 = now_ms();
	fused = malloc((size_t)W * H);
	if (!fused)
		die("malloc fused");
	for (i = 0; i < nexp; i++)
		colsum[i] = calloc(W, sizeof(unsigned int));

	for (j = 0; j < H; j++) {
		float *num = calloc(W, sizeof(float));
		float *den = calloc(W, sizeof(float));
		if (!num || !den)
			die("malloc row");
		/* 维护 2R+1 行的列和 */
		for (i = 0; i < nexp; i++) {
			if (j == 0) {
				int k;
				for (k = 0; k <= RADIUS && k < H; k++) {
					unsigned int *r = ts[i] + (size_t)k * W;
					int x;
					for (x = 0; x < W; x++)
						colsum[i][x] += r[x];
				}
			} else {
				int add = j + RADIUS, del = j - RADIUS - 1;
				int x;
				if (add < H) {
					unsigned int *r = ts[i] + (size_t)add * W;
					for (x = 0; x < W; x++)
						colsum[i][x] += r[x];
				}
				if (del >= 0) {
					unsigned int *r = ts[i] + (size_t)del * W;
					for (x = 0; x < W; x++)
						colsum[i][x] -= r[x];
				}
			}
		}
		for (i = 0; i < nexp; i++) {
			const unsigned char *row = ys[i] + (size_t)j * W;
			int x;
			for (x = 0; x < W; x++) {
				float w = (float)colsum[i][x];
				num[x] += w * g_lin[row[x]];
				den[x] += w;
			}
		}
		for (i = 0; i < W; i++) {
			float v = den[i] > 0 ? num[i] / den[i] : 0;
			int idx = (int)(v * 4096.0f);
			if (idx < 0) idx = 0;
			if (idx > 4096) idx = 4096;
			fused[(size_t)j * W + i] = (unsigned char)(g_degamma[idx] + 0.5f);
		}
		free(num);
		free(den);
	}
	t_fuse = now_ms() - t0;
	t_all = now_ms() - t_all;

	/* 可选：把每档输入也存下来，方便对比 */
	if (keep) {
		int k;
		for (k = 0; k < nexp; k++) {
			char name[512];
			snprintf(name, sizeof(name), "%s.e%d.nv12", outpath, exposures[k]);
			FILE *fp = fopen(name, "wb");
			if (!fp)
				continue;
			fwrite(ys[k], 1, (size_t)W * H, fp);
			if (uv_mid)
				fwrite(uv_mid, 1, (size_t)W * H / 2, fp);
			fclose(fp);
			printf(">>> 输入已存: %s\n", name);
		}
	}

	FILE *fp = fopen(outpath, "wb");
	if (!fp)
		die("fopen out");
	fwrite(fused, 1, (size_t)W * H, fp);
	if (uv_mid)
		fwrite(uv_mid, 1, (size_t)W * H / 2, fp);
	else {
		unsigned char *z = calloc(1, (size_t)W * H / 2);
		fwrite(z, 1, (size_t)W * H / 2, fp);
		free(z);
	}
	fclose(fp);

	printf(">>> 耗时: 采集 %.0f ms | 权重 %.0f ms | 平滑 %.0f ms | 融合 %.0f ms | 合计 %.0f ms\n",
	       t_cap, t_w, t_blur, t_fuse, t_all);
	printf(">>> 融合单帧净耗时(不含采集): %.0f ms\n", t_w + t_blur + t_fuse);
	printf(">>> 输出: %s\n", outpath);
	return 0;
}
