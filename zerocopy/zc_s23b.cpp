/*
 * zc_s23b.cpp — S2(全帧比对) + S3(RGA 缩放,两条 API 路径 + CPU 参考)
 *
 * 背景:板上内核是老的 rga2 驱动,用户态 librga 是 1.9.2,im2d 可能不被接受,
 *       所以这里两条路都试:
 *         path A: 直接填 rga_buffer_t + imresize()      (新接口)
 *         path B: rga_info_t + c_RkRgaBlit()            (老接口,匹配 rga2 驱动)
 *
 * 编译:g++ -O2 -o zc_s23b zc_s23b.cpp -I/usr/include/rga -lrga
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <time.h>

#include <rga/im2d.h>
#include <rga/RgaApi.h>
#include <rga/rga.h>
#include <rga/drmrga.h>

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

static void cpu_ref_nv12_to_rgb(const uint8_t *nv12, int sw, int sh,
				uint8_t *rgb, int dw, int dh)
{
	const uint8_t *Y = nv12;
	const uint8_t *UV = nv12 + (size_t)sw * sh;
	for (int y = 0; y < dh; y++) {
		int sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh;
		if (sy1 <= sy0) sy1 = sy0 + 1;
		for (int x = 0; x < dw; x++) {
			int sx0 = x * sw / dw, sx1 = (x + 1) * sw / dw;
			if (sx1 <= sx0) sx1 = sx0 + 1;
			unsigned long sum = 0; int n = 0;
			for (int yy = sy0; yy < sy1; yy++)
				for (int xx = sx0; xx < sx1; xx++, n++)
					sum += Y[(size_t)yy * sw + xx];
			int luma = n ? (int)(sum / n) : 0;
			int cx = (sx0 + sx1) / 2, cy = (sy0 + sy1) / 2;
			const uint8_t *uv = UV + (size_t)(cy / 2) * sw + (cx / 2) * 2;
			/* limited range(BT.601 studio swing):RGA 用的就是这套,
			 * Y 16..235 -> 0..255,C 16..240 */
			int yv = (luma - 16) * 255 / 219;
			int cb = (uv[0] - 128) * 255 / 224;
			int cr = (uv[1] - 128) * 255 / 224;
			int r = yv + ((359 * cr) >> 8);
			int g = yv - ((88 * cb + 183 * cr) >> 8);
			int b = yv + ((454 * cb) >> 8);
			r = r < 0 ? 0 : (r > 255 ? 255 : r);
			g = g < 0 ? 0 : (g > 255 ? 255 : g);
			b = b < 0 ? 0 : (b > 255 ? 255 : b);
			uint8_t *o = rgb + ((size_t)y * dw + x) * 3;
			o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b;
		}
	}
}

/* ---- path A:新接口,直接填结构体 ---- */
static IM_STATUS rga_path_a(int src_fd, int sw, int sh, uint8_t *dst_buf, int dw, int dh)
{
	rga_buffer_t src; memset(&src, 0, sizeof src);
	src.fd = src_fd; src.width = sw; src.height = sh;
	src.wstride = sw; src.hstride = sh; src.format = RK_FORMAT_YCbCr_420_SP;

	rga_buffer_t dst; memset(&dst, 0, sizeof dst);
	dst.vir_addr = dst_buf; dst.width = dw; dst.height = dh;
	dst.wstride = dw; dst.hstride = dh; dst.format = RK_FORMAT_RGB_888;

	return imresize(src, dst);
}

/* ---- path B:老接口 c_RkRgaBlit ---- */
static int rga_path_b(int src_fd, int sw, int sh, uint8_t *dst_buf, int dw, int dh, int bufsize)
{
	rga_info_t src; memset(&src, 0, sizeof src);
	src.fd = src_fd;
	src.format = RK_FORMAT_YCbCr_420_SP;
	src.rect.xoffset = 0; src.rect.yoffset = 0;
	src.rect.width = sw; src.rect.height = sh;
	src.rect.wstride = sw; src.rect.hstride = sh;
	src.rect.format = RK_FORMAT_YCbCr_420_SP;
	src.mmuFlag = 1;
	src.bufferSize = bufsize;

	rga_info_t dst; memset(&dst, 0, sizeof dst);
	dst.virAddr = dst_buf;
	dst.format = RK_FORMAT_RGB_888;
	dst.rect.xoffset = 0; dst.rect.yoffset = 0;
	dst.rect.width = dw; dst.rect.height = dh;
	dst.rect.wstride = dw; dst.rect.hstride = dh;
	dst.rect.format = RK_FORMAT_RGB_888;
	dst.mmuFlag = 1;

	return c_RkRgaBlit(&src, &dst, NULL);
}

/* ---- path C:老接口 + 虚拟地址(vb2 mmap 的地址,同样是零拷贝) ---- */
static int rga_path_c(void *src_va, int sw, int sh, uint8_t *dst_buf, int dw, int dh, int bufsize)
{
	rga_info_t src; memset(&src, 0, sizeof src);
	src.virAddr = src_va;
	src.format = RK_FORMAT_YCbCr_420_SP;
	src.rect.xoffset = 0; src.rect.yoffset = 0;
	src.rect.width = sw; src.rect.height = sh;
	src.rect.wstride = sw; src.rect.hstride = sh;
	src.rect.format = RK_FORMAT_YCbCr_420_SP;
	src.mmuFlag = 1;
	src.bufferSize = bufsize;

	rga_info_t dst; memset(&dst, 0, sizeof dst);
	dst.virAddr = dst_buf;
	dst.format = RK_FORMAT_RGB_888;
	dst.rect.xoffset = 0; dst.rect.yoffset = 0;
	dst.rect.width = dw; dst.rect.height = dh;
	dst.rect.wstride = dw; dst.rect.hstride = dh;
	dst.rect.format = RK_FORMAT_RGB_888;
	dst.mmuFlag = 1;
	return c_RkRgaBlit(&src, &dst, NULL);
}


/* ---- 自检:RGA 能否处理普通用户内存(malloc 的 NV12 -> RGB) ---- */
static int selftest(void)
{
	int sw = 640, sh = 480, dw = 320, dh = 240;
	uint8_t *src = (uint8_t *)calloc(1, (size_t)sw * sh * 3 / 2);
	uint8_t *dst = (uint8_t *)calloc(1, (size_t)dw * dh * 3);
	memset(src, 0, (size_t)sw * sh * 3 / 2);
	memset(src, 100, (size_t)sw * sh);              /* Y = 100 */
	memset(src + (size_t)sw * sh, 128, (size_t)sw * sh / 2);  /* UV = 128 */

	printf("=== RGA 自检(malloc 内存, Y=100 灰面) ===\n");
	int rC = rga_path_c(src, sw, sh, dst, dw, dh, (int)((size_t)sw * sh * 3 / 2));
	double mean = 0; for (size_t i = 0; i < (size_t)dw * dh * 3; i++) mean += dst[i];
	mean /= (size_t)dw * dh * 3;
	printf("path C(va)  ret=%d  输出均值 %.1f (期望 ~100)\n", rC, mean);

	IM_STATUS stA = rga_path_a(-1, sw, sh, dst, dw, dh);   /* fd 无效,应失败 */
	printf("path A(fd=-1) -> %s (期望失败)\n", imStrError(stA));

	/* 再用 im2d 的 wrapbuffer_virtualaddr 试一次 */
	rga_buffer_t s2 = wrapbuffer_virtualaddr(src, sw, sh, RK_FORMAT_YCbCr_420_SP);
	rga_buffer_t d2 = wrapbuffer_virtualaddr(dst, dw, dh, RK_FORMAT_RGB_888);
	IM_STATUS st2 = imresize(s2, d2);
	mean = 0; for (size_t i = 0; i < (size_t)dw * dh * 3; i++) mean += dst[i];
	mean /= (size_t)dw * dh * 3;
	printf("im2d va->va  %s  输出均值 %.1f\n", imStrError(st2), mean);
	printf("自检结论: %s\n", (mean > 50 && mean < 150) ? "RGA 可用 ✓" : "RGA 输出异常 ✗");
	/* 再测大尺寸:3840x2160 与 1920x1080(判断是不是分辨率/缓冲体积限制) */
	{
		int W = 3840, H = 2160;
		size_t sz = (size_t)W * H * 3 / 2;
		uint8_t *big = (uint8_t *)calloc(1, sz);
		memset(big, 100, (size_t)W * H);
		memset(big + (size_t)W * H, 128, (size_t)W * H / 2);
		uint8_t *out = (uint8_t *)calloc(1, (size_t)dw * dh * 3);
		int r = rga_path_c(big, W, H, out, dw, dh, (int)sz);
		double m = 0; for (size_t i = 0; i < (size_t)dw * dh * 3; i++) m += out[i];
		m /= (size_t)dw * dh * 3;
		printf("path C 3840x2160 -> 640x640 : ret=%d 均值 %.1f (期望 ~100)\n", r, m);
		free(big); free(out);

		W = 1920; H = 1080; sz = (size_t)W * H * 3 / 2;
		uint8_t *mid = (uint8_t *)calloc(1, sz);
		memset(mid, 100, (size_t)W * H);
		memset(mid + (size_t)W * H, 128, (size_t)W * H / 2);
		out = (uint8_t *)calloc(1, (size_t)dw * dh * 3);
		r = rga_path_c(mid, W, H, out, dw, dh, (int)sz);
		m = 0; for (size_t i = 0; i < (size_t)dw * dh * 3; i++) m += out[i];
		m /= (size_t)dw * dh * 3;
		printf("path C 1920x1080 -> 640x640 : ret=%d 均值 %.1f (期望 ~100)\n", r, m);
		free(mid); free(out);
	}

	free(src); free(dst);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/video5";
	int sw = (argc > 2) ? atoi(argv[2]) : 3840;
	int sh = (argc > 3) ? atoi(argv[3]) : 2160;
	int dw = (argc > 4) ? atoi(argv[4]) : 640;
	int dh = (argc > 5) ? atoi(argv[5]) : 640;

	if (strcmp(dev, "selftest") == 0) return selftest();

	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }

	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
	fmt.fmt.pix_mp.width = sw; fmt.fmt.pix_mp.height = sh;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	sw = fmt.fmt.pix_mp.width; sh = fmt.fmt.pix_mp.height;
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

	int idx = -1;
	for (int k = 0; k < 2; k++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.m.planes = p; b.length = nplanes;
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); return 1; }
		idx = b.index;
		if (k == 0 && xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF back"); return 1; }
	}

	struct v4l2_exportbuffer exp; memset(&exp, 0, sizeof exp);
	exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	exp.index = idx; exp.plane = 0; exp.flags = O_RDWR | O_CLOEXEC;
	if (xioctl(fd, VIDIOC_EXPBUF, &exp) < 0) { perror("EXPBUF"); return 1; }
	printf("frame index=%d  len=%zu  expbuf fd=%d\n", idx, len[idx], exp.fd);

	/* ================= S2 ================= */
	void *dm = mmap(NULL, len[idx], PROT_READ | PROT_WRITE, MAP_SHARED, exp.fd, 0);
	if (dm == MAP_FAILED) { perror("mmap(dmabuf)"); return 1; }
	const uint8_t *a = (const uint8_t *)mem[idx], *b2 = (const uint8_t *)dm;
	uint64_t diff = 0;
	double t0 = now_ms();
	for (size_t i = 0; i < len[idx]; i++) if (a[i] != b2[i]) diff++;
	double t1 = now_ms();
	printf("\n===== S2: dmabuf vs vb2 mmap =====\n");
	printf("逐字节比对 : %s  (%.1f ms, %zu 字节)\n",
	       diff == 0 ? "完全一致 ✓" : "有差异 ✗", t1 - t0, len[idx]);
	printf("不同字节数 : %llu\n", (unsigned long long)diff);
	printf("S2 %s\n", diff == 0 ? "PASS ✓" : "FAIL ✗");

	/* ================= S3 ================= */
	size_t dst_size = (size_t)dw * dh * 3;
	uint8_t *rgb_cpu = (uint8_t *)calloc(1, dst_size);

	double t_cpu0 = now_ms();
	cpu_ref_nv12_to_rgb((const uint8_t *)mem[idx], sw, sh, rgb_cpu, dw, dh);
	double t_cpu1 = now_ms();

	printf("\n===== S3: RGA 缩放 %dx%d NV12 -> %dx%d RGB888 =====\n", sw, sh, dw, dh);
	printf("CPU 参考(盒式平均+色彩转换): %.2f ms\n", t_cpu1 - t_cpu0);

	struct { const char *name; uint8_t *buf; int ok; double ms; } paths[3];
	paths[0] = {"A: imresize + fd",      (uint8_t *)calloc(1, dst_size), 0, 0};
	paths[1] = {"B: c_RkRgaBlit + fd",   (uint8_t *)calloc(1, dst_size), 0, 0};
	paths[2] = {"C: c_RkRgaBlit + va",   (uint8_t *)calloc(1, dst_size), 0, 0};

	IM_STATUS stA = rga_path_a(exp.fd, sw, sh, paths[0].buf, dw, dh);
	paths[0].ok = (stA == IM_STATUS_SUCCESS || stA == IM_STATUS_NOERROR);
	int rB = rga_path_b(exp.fd, sw, sh, paths[1].buf, dw, dh, (int)len[idx]);
	paths[1].ok = (rB == 0);
	int rC = rga_path_c(mem[idx], sw, sh, paths[2].buf, dw, dh, (int)len[idx]);
	paths[2].ok = (rC == 0);

	const int REP = 50;
	for (int i = 0; i < 3; i++) {
		if (!paths[i].ok) continue;
		double s0 = now_ms();
		for (int k = 0; k < REP; k++) {
			if (i == 0) rga_path_a(exp.fd, sw, sh, paths[i].buf, dw, dh);
			else if (i == 1) rga_path_b(exp.fd, sw, sh, paths[i].buf, dw, dh, (int)len[idx]);
			else rga_path_c(mem[idx], sw, sh, paths[i].buf, dw, dh, (int)len[idx]);
		}
		paths[i].ms = (now_ms() - s0) / REP;
	}

	for (int i = 0; i < 3; i++) {
		double mean = 0, mse = 0;
		for (size_t k = 0; k < dst_size; k++) mean += paths[i].buf[k];
		mean /= dst_size;
		for (size_t k = 0; k < dst_size; k++) {
			double d = (double)paths[i].buf[k] - (double)rgb_cpu[k];
			mse += d * d;
		}
		mse /= dst_size;
		double psnr = (mse <= 0) ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
		printf("%-22s : %s  单次 %.2f ms  均值 %6.1f  PSNR %6.2f dB\n",
		       paths[i].name, paths[i].ok ? "OK " : "FAIL", paths[i].ms, mean, psnr);
		if (paths[i].ok) {
			char fn[64]; snprintf(fn, sizeof fn, "/root/s3_%c.ppm", 'A' + i);
			FILE *f = fopen(fn, "wb");
			if (f) { fprintf(f, "P6\n%d %d\n255\n", dw, dh);
				 fwrite(paths[i].buf, 1, dst_size, f); fclose(f); }
		}
	}

	/* path D:先 memcpy 到普通内存再做 RGA(判断 RGA 是否读不了 vb2 缓冲) */
	{
		uint8_t *cp = (uint8_t *)malloc(len[idx]);
		uint8_t *rgb_d = (uint8_t *)calloc(1, dst_size);
		double c0 = now_ms();
		memcpy(cp, mem[idx], len[idx]);
		double c1 = now_ms();
		int rD = rga_path_c(cp, sw, sh, rgb_d, dw, dh, (int)len[idx]);
		double mean = 0, mse = 0;
		for (size_t k = 0; k < dst_size; k++) mean += rgb_d[k];
		mean /= dst_size;
		for (size_t k = 0; k < dst_size; k++) {
			double d = (double)rgb_d[k] - (double)rgb_cpu[k]; mse += d * d;
		}
		mse /= dst_size;
		double psnr = (mse <= 0) ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
		printf("D: memcpy(%.1fms) + RGA   : %s  均值 %6.1f  PSNR %6.2f dB\n",
		       c1 - c0, rD == 0 ? "OK " : "FAIL", mean, psnr);
		FILE *fd2 = fopen("/root/s3_D.ppm", "wb");
		if (fd2) { fprintf(fd2, "P6\n%d %d\n255\n", dw, dh); fwrite(rgb_d, 1, dst_size, fd2); fclose(fd2); }
		free(cp); free(rgb_d);
	}

	double cm = 0; for (size_t k = 0; k < dst_size; k++) cm += rgb_cpu[k]; cm /= dst_size;
	printf("CPU 参考均值           : %.1f\n", cm);
	FILE *f = fopen("/root/s3_cpu.ppm", "wb");
	if (f) { fprintf(f, "P6\n%d %d\n255\n", dw, dh); fwrite(rgb_cpu, 1, dst_size, f); fclose(f); }
	printf("PPM 已存 /root/s3_{A,B,C,cpu}.ppm\n");

	munmap(dm, len[idx]);
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	close(exp.fd);
	for (unsigned i = 0; i < req.count; i++) munmap(mem[i], len[i]);
	close(fd);
	free(rgb_cpu);
	for (int i = 0; i < 3; i++) free(paths[i].buf);
	return 0;
}
