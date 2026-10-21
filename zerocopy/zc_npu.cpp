/*
 * zc_npu.cpp — S4:三种数据通路 + yolov8n 推理
 *
 *   mode A (copy) : mmap 采集 -> memcpy 12.4MB -> RGA -> rknn_inputs_set(再拷 1.2MB) -> run
 *   mode B (rga)  : RGA 直接从 vb2 虚拟地址 -> rknn_inputs_set(拷 1.2MB) -> run
 *   mode C (zc)   : EXPBUF dmabuf fd -> RGA 直接写进 rknn 输入 tensor -> rknn_set_io_mem -> run
 *
 * 编译:
 *   g++ -O2 -o zc_npu zc_npu.cpp -I/usr/include/rga -I/root/npu/3rdparty/rknpu2/include -lrga -lrknnrt
 *
 * 用法: ./zc_npu [mode:a|b|c] [frames] [device]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <time.h>
#include <sys/resource.h>

#include <rga/im2d.h>
#include <rga/RgaApi.h>
#include <rga/rga.h>
#include <rga/drmrga.h>
#include "rknn_api.h"

#define NBUF 4
#define MODEL "/root/npu/examples/yolov8/model/yolov8n_rk3568_i8.rknn"
#define OUT_W 640
#define OUT_H 640
#define OBJ_THRESH 0.35f
#define NMS_THRESH 0.45f

static unsigned long long g_copied = 0;      /* 累计 memcpy 字节数 */
static unsigned long long g_copy_calls = 0;
static void cpy(void *d, const void *s, size_t n)
{
	memcpy(d, s, n);
	g_copied += n; g_copy_calls++;
}

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

/* ---------------- 结果框 ---------------- */
struct box { float x1, y1, x2, y2, score; int cls; };

static float iou(const box &a, const box &b)
{
	float xx1 = std::max(a.x1, b.x1), yy1 = std::max(a.y1, b.y1);
	float xx2 = std::min(a.x2, b.x2), yy2 = std::min(a.y2, b.y2);
	float w = std::max(0.f, xx2 - xx1), h = std::max(0.f, yy2 - yy1);
	float inter = w * h;
	float ua = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
	return ua > 0 ? inter / ua : 0;
}

static void nms(std::vector<box> &v)
{
	std::sort(v.begin(), v.end(), [](const box &a, const box &b) { return a.score > b.score; });
	std::vector<box> keep;
	for (auto &b : v) {
		bool drop = false;
		for (auto &k : keep) if (iou(b, k) > NMS_THRESH) { drop = true; break; }
		if (!drop) keep.push_back(b);
	}
	v.swap(keep);
}

/* ---------------- yolov8 后处理(9 输出:3 尺度 x [64,80,1]) ---------------- */
static int decode_yolov8(rknn_tensor_attr *oa, int8_t **odata, std::vector<box> &res)
{
	static const int strides[3] = {8, 16, 32};
	int n = 0;
	for (int s = 0; s < 3; s++) {
		rknn_tensor_attr &areg = oa[s * 3 + 0];
		rknn_tensor_attr &acls = oa[s * 3 + 1];
		rknn_tensor_attr &ascr = oa[s * 3 + 2];
		int gh = areg.dims[2], gw = areg.dims[3];
		if (acls.dims[2] != gh || ascr.dims[2] != gh) continue;
		int stride = strides[s];
		int8_t *reg = odata[s * 3 + 0];
		int8_t *cls = odata[s * 3 + 1];
		int8_t *scr = odata[s * 3 + 2];

		for (int y = 0; y < gh; y++) {
			for (int x = 0; x < gw; x++) {
				int idx = y * gw + x;
				float score = (scr[idx] - ascr.zp) * ascr.scale;
				if (score < OBJ_THRESH) continue;
				/* 类别 */
				int best = -1; float bestv = 0;
				for (int c = 0; c < 80; c++) {
					float v = (cls[c * gh * gw + idx] - acls.zp) * acls.scale;
					if (v > bestv) { bestv = v; best = c; }
				}
				if (best < 0) continue;
				float conf = score * bestv;
				if (conf < OBJ_THRESH) continue;
				/* DFL: 4 边 x 16 桶 */
				float d[4] = {0, 0, 0, 0};
				for (int side = 0; side < 4; side++) {
					float p[16], mx = -1e9f, sum = 0;
					for (int k = 0; k < 16; k++) {
						float v = (reg[(side * 16 + k) * gh * gw + idx] - areg.zp) * areg.scale;
						p[k] = v; if (v > mx) mx = v;
					}
					for (int k = 0; k < 16; k++) { p[k] = expf(p[k] - mx); sum += p[k]; }
					float acc = 0;
					for (int k = 0; k < 16; k++) acc += (p[k] / sum) * k;
					d[side] = acc;
				}
				float cx = (x + 0.5f) * stride, cy = (y + 0.5f) * stride;
				box b;
				b.x1 = cx - d[0] * stride; b.y1 = cy - d[1] * stride;
				b.x2 = cx + d[2] * stride; b.y2 = cy + d[3] * stride;
				b.score = conf; b.cls = best;
				res.push_back(b);
			}
		}
	}
	n = (int)res.size();
	nms(res);
	return (int)res.size();
}

static void draw_boxes(uint8_t *rgb, int w, int h, const std::vector<box> &v)
{
	for (auto &b : v) {
		int x1 = std::max(0, (int)b.x1), y1 = std::max(0, (int)b.y1);
		int x2 = std::min(w - 1, (int)b.x2), y2 = std::min(h - 1, (int)b.y2);
		for (int x = x1; x <= x2; x++) {
			for (int t = 0; t < 2; t++) {
				if (y1 + t < h) { uint8_t *p = rgb + ((size_t)(y1 + t) * w + x) * 3; p[0] = 255; p[1] = 0; p[2] = 0; }
				if (y2 - t >= 0) { uint8_t *p = rgb + ((size_t)(y2 - t) * w + x) * 3; p[0] = 255; p[1] = 0; p[2] = 0; }
			}
		}
		for (int y = y1; y <= y2; y++) {
			for (int t = 0; t < 2; t++) {
				if (x1 + t < w) { uint8_t *p = rgb + ((size_t)y * w + x1 + t) * 3; p[0] = 255; p[1] = 0; p[2] = 0; }
				if (x2 - t >= 0) { uint8_t *p = rgb + ((size_t)y * w + x2 - t) * 3; p[0] = 255; p[1] = 0; p[2] = 0; }
			}
		}
	}
}

int main(int argc, char **argv)
{
	char mode = (argc > 1) ? argv[1][0] : 'a';
	int frames = (argc > 2) ? atoi(argv[2]) : 30;
	const char *dev = (argc > 3) ? argv[3] : "/dev/video5";

	printf("=== zc_npu: mode=%c frames=%d dev=%s ===\n", mode, frames, dev);

	/* ---------- 模型 ---------- */
	FILE *mf = fopen(MODEL, "rb");
	if (!mf) { perror("open model"); return 1; }
	fseek(mf, 0, SEEK_END); long msz = ftell(mf); fseek(mf, 0, SEEK_SET);
	void *model = malloc(msz);
	if (fread(model, 1, msz, mf) != (size_t)msz) return 1;
	fclose(mf);

	rknn_context ctx = 0;
	int ret = rknn_init(&ctx, model, msz, 0, NULL);
	if (ret < 0) { printf("rknn_init fail %d\n", ret); return 1; }

	rknn_input_output_num io_num;
	rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof io_num);

	rknn_tensor_attr in_attr; memset(&in_attr, 0, sizeof in_attr); in_attr.index = 0;
	rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof in_attr);

	unsigned nout = io_num.n_output;
	rknn_tensor_attr *oa = (rknn_tensor_attr *)calloc(nout, sizeof(rknn_tensor_attr));
	for (unsigned i = 0; i < nout; i++) { oa[i].index = i; rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &oa[i], sizeof(rknn_tensor_attr)); }
	printf("model: in %ux%ux%u %s, out %u\n", in_attr.dims[1], in_attr.dims[2], in_attr.dims[3],
	       in_attr.type == RKNN_TENSOR_INT8 ? "INT8" : "?", nout);

	/* native 输入属性(pass_through=1):零拷贝必须用这个,否则 runtime 每次还要转换 */
	rknn_tensor_attr in_native; memset(&in_native, 0, sizeof in_native); in_native.index = 0;
	int rq = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &in_native, sizeof in_native);
	if (rq == 0) {
		printf("native input attr: dims=[%u,%u,%u,%u] type=%d passthrough=%u size_with_stride=%u\n",
		       in_native.dims[0], in_native.dims[1], in_native.dims[2], in_native.dims[3],
		       in_native.type, in_native.pass_through, in_native.size_with_stride);
	} else {
		printf("native attr query ret=%d, 退回普通 attr\n", rq);
		in_native = in_attr;
	}

	/* 零拷贝输入内存(模式 C 用) */
	rknn_tensor_mem *in_mem = NULL;
	if (mode == 'c' || mode == 'g') {
		in_mem = rknn_create_mem(ctx, in_native.size_with_stride);
		if (!in_mem) { printf("rknn_create_mem fail\n"); return 1; }
		in_native.pass_through = 1;
		ret = rknn_set_io_mem(ctx, in_mem, &in_native);
		printf("rknn_set_io_mem(input) ret=%d mem virt=%p fd=%d size=%u\n",
		       ret, in_mem->virt_addr, in_mem->fd, in_mem->size);
	}

	/* 输出内存(模式 C 需要;A/B 用 rknn_outputs_get) */
	rknn_tensor_mem **out_mems = NULL;
	if (mode == 'c' || mode == 'g') {
		out_mems = (rknn_tensor_mem **)calloc(nout, sizeof(void *));
		for (unsigned i = 0; i < nout; i++) {
			out_mems[i] = rknn_create_mem(ctx, oa[i].size_with_stride);
			int r2 = rknn_set_io_mem(ctx, out_mems[i], &oa[i]);
			if (r2 < 0) printf("set_io_mem(out %u) fail %d\n", i, r2);
		}
	}

	/* ---------- 文件模式 g:验证零拷贝输入路径(写进 rknn 输入 tensor) ---------- */
	if (mode == 'g') {
		const char *fp = (argc > 4) ? argv[4] : "/root/out_640.rgb";
		FILE *rf = fopen(fp, "rb");
		if (!rf) { perror("open raw"); return 1; }
		size_t need = (size_t)OUT_W * OUT_H * 3;
		uint8_t *rgb = (uint8_t *)malloc(need);
		if (fread(rgb, 1, need, rf) != need) { printf("raw size mismatch\n"); return 1; }
		fclose(rf);
		int8_t *dstt = (int8_t *)in_mem->virt_addr;
		struct rusage g0, g1; getrusage(RUSAGE_SELF, &g0);
		double w0 = now_ms();
		int total = 0;
		for (int rep = 0; rep < frames; rep++) {
			uint64_t *t64 = (uint64_t *)dstt;
			size_t n64 = need / 8;
			const uint64_t mask = 0x8080808080808080ULL;
			for (size_t k = 0; k < n64; k++) t64[k] = ((uint64_t *)rgb)[k] ^ mask;
			rknn_run(ctx, NULL);
			std::vector<box> boxes;
			int8_t *ptrs[32];
			for (unsigned i = 0; i < nout; i++) ptrs[i] = (int8_t *)out_mems[i]->virt_addr;
			decode_yolov8(oa, ptrs, boxes);
			total += (int)boxes.size();
		}
		getrusage(RUSAGE_SELF, &g1);
		double w1 = now_ms();
		double cpu = (g1.ru_utime.tv_sec - g0.ru_utime.tv_sec) * 1000.0
		           + (g1.ru_utime.tv_usec - g0.ru_utime.tv_usec) / 1000.0
		           + (g1.ru_stime.tv_sec - g0.ru_stime.tv_sec) * 1000.0
		           + (g1.ru_stime.tv_usec - g0.ru_stime.tv_usec) / 1000.0;
		printf("g(零拷贝): %d 次  墙钟 %.1f ms  %.1f inf/s  CPU %.1f ms/次  框/帧 %.1f\n",
		       frames, w1 - w0, frames * 1000.0 / (w1 - w0), cpu / frames, (double)total / frames);
		return 0;
	}

	/* ---------- 文件模式:直接用一张 640x640 RGB 原始图验证解码器 ---------- */
	if (mode == 'f') {
		const char *fp = (argc > 4) ? argv[4] : "/root/out_640.rgb";
		FILE *rf = fopen(fp, "rb");
		if (!rf) { perror("open raw"); return 1; }
		size_t need = (size_t)OUT_W * OUT_H * 3;
		uint8_t *rgb = (uint8_t *)malloc(need);
		if (fread(rgb, 1, need, rf) != need) { printf("raw size mismatch\n"); return 1; }
		fclose(rf);
		struct rusage f0, f1; getrusage(RUSAGE_SELF, &f0);
		double w0 = now_ms();
		int total = 0; size_t last_boxes = 0;
		for (int rep = 0; rep < frames; rep++) {
			rknn_input in; memset(&in, 0, sizeof in);
			in.index = 0; in.type = RKNN_TENSOR_UINT8; in.fmt = RKNN_TENSOR_NHWC;
			in.size = (uint32_t)need; in.buf = rgb;
			rknn_inputs_set(ctx, 1, &in);
			rknn_run(ctx, NULL);
			rknn_output *outs = (rknn_output *)calloc(nout, sizeof(rknn_output));
			for (unsigned i = 0; i < nout; i++) { outs[i].index = i; outs[i].want_float = 0; }
			rknn_outputs_get(ctx, nout, outs, NULL);
			int8_t *ptrs[32];
			for (unsigned i = 0; i < nout; i++) ptrs[i] = (int8_t *)outs[i].buf;
			std::vector<box> boxes;
			decode_yolov8(oa, ptrs, boxes);
			total += (int)boxes.size(); last_boxes = boxes.size();
			rknn_outputs_release(ctx, nout, outs);
			free(outs);
		}
		getrusage(RUSAGE_SELF, &f1);
		double w1 = now_ms();
		double cpu = (f1.ru_utime.tv_sec - f0.ru_utime.tv_sec) * 1000.0
		           + (f1.ru_utime.tv_usec - f0.ru_utime.tv_usec) / 1000.0
		           + (f1.ru_stime.tv_sec - f0.ru_stime.tv_sec) * 1000.0
		           + (f1.ru_stime.tv_usec - f0.ru_stime.tv_usec) / 1000.0;
		printf("f(copy): %d 次  墙钟 %.1f ms  %.1f inf/s  CPU %.1f ms/次  框/帧 %.1f\n",
		       frames, w1 - w0, frames * 1000.0 / (w1 - w0), cpu / frames, (double)total / frames);
		return 0;
	}

	/* ---------- 采集 ---------- */
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open video"); return 1; }
	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(fd, VIDIOC_G_FMT, &fmt);
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	xioctl(fd, VIDIOC_S_FMT, &fmt);
	int sw = fmt.fmt.pix_mp.width, sh = fmt.fmt.pix_mp.height;
	unsigned nplanes = fmt.fmt.pix_mp.num_planes;
	printf("capture %dx%d NV12\n", sw, sh);

	struct v4l2_requestbuffers req; memset(&req, 0, sizeof req);
	req.count = NBUF; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
	xioctl(fd, VIDIOC_REQBUFS, &req);
	void *mem[NBUF] = {0}; size_t len[NBUF] = {0};
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = nplanes;
		xioctl(fd, VIDIOC_QUERYBUF, &b);
		len[i] = p[0].length;
		mem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, p[0].m.mem_offset);
	}
	/* 一次性导出所有 buffer 的 dmabuf fd(模式 C 用):
	 * 原来每帧都 VIDIOC_EXPBUF + close,内核要做 dma-buf attach/detach,
	 * 实测 preproc 比 B 多 ~9ms;改成 setup 阶段导一次、循环里直接用。 */
	static int exp_fds[NBUF];
	for (int i = 0; i < NBUF; i++) exp_fds[i] = -1;
	if (mode == 'c') {
		double te0 = now_ms();
		for (unsigned i = 0; i < req.count; i++) {
			struct v4l2_exportbuffer e; memset(&e, 0, sizeof e);
			e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			e.index = i; e.plane = 0; e.flags = O_RDWR | O_CLOEXEC;
			if (xioctl(fd, VIDIOC_EXPBUF, &e) == 0) exp_fds[i] = e.fd;
			else perror("EXPBUF(setup)");
		}
		printf("预先导出 %u 个 dmabuf(耗时 %.2f ms,一次性)\n", req.count, now_ms() - te0);
	}

	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = nplanes;
		xioctl(fd, VIDIOC_QBUF, &b);
	}
	enum v4l2_buf_type btype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(fd, VIDIOC_STREAMON, &btype);

	/* ---------- 缓冲 ---------- */
	uint8_t *host_rgb = (uint8_t *)calloc(1, (size_t)OUT_W * OUT_H * 3);   /* A/B 用 */
	uint8_t *nv12_copy = (uint8_t *)malloc(len[0]);                        /* A 用 */

	/* 累加计时 + CPU 占用 */
	struct rusage ru0, ru1;
	getrusage(RUSAGE_SELF, &ru0);
	double wall0 = now_ms();
	double t_cap = 0, t_pre = 0, t_run = 0, t_post = 0;
	int total_boxes = 0;

	for (int f = 0; f < frames; f++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.m.planes = p; b.length = nplanes;
		double t0 = now_ms();
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
		double t1 = now_ms();
		t_cap += t1 - t0;

		int idx = b.index;
		const uint8_t *src_va = (const uint8_t *)mem[idx];
		int exp_fd = -1;

		/* ---- 预处理 ---- */
		if (mode == 'a') {
			cpy(nv12_copy, src_va, len[idx]);          /* 12.4MB 拷贝 */
			rga_buffer_t s; memset(&s, 0, sizeof s);
			s.vir_addr = nv12_copy; s.width = sw; s.height = sh;
			s.wstride = sw; s.hstride = sh; s.format = RK_FORMAT_YCbCr_420_SP;
			rga_buffer_t d; memset(&d, 0, sizeof d);
			d.vir_addr = host_rgb; d.width = OUT_W; d.height = OUT_H;
			d.wstride = OUT_W; d.hstride = OUT_H; d.format = RK_FORMAT_RGB_888;
			imresize(s, d);
		} else if (mode == 'b') {
			rga_buffer_t s; memset(&s, 0, sizeof s);
			s.vir_addr = (void *)src_va; s.width = sw; s.height = sh;
			s.wstride = sw; s.hstride = sh; s.format = RK_FORMAT_YCbCr_420_SP;
			rga_buffer_t d; memset(&d, 0, sizeof d);
			d.vir_addr = host_rgb; d.width = OUT_W; d.height = OUT_H;
			d.wstride = OUT_W; d.hstride = OUT_H; d.format = RK_FORMAT_RGB_888;
			imresize(s, d);
		} else {
			/* C:用 setup 阶段导出的 dmabuf fd,RGA 直接写进 rknn 输入 tensor */
			if (exp_fds[idx] >= 0) {
				exp_fd = exp_fds[idx];
				rga_buffer_t s; memset(&s, 0, sizeof s);
				s.fd = exp_fd; s.width = sw; s.height = sh;
				s.wstride = sw; s.hstride = sh; s.format = RK_FORMAT_YCbCr_420_SP;
				rga_buffer_t d; memset(&d, 0, sizeof d);
				d.fd = in_mem->fd; d.width = OUT_W; d.height = OUT_H;
				d.wstride = OUT_W; d.hstride = OUT_H; d.format = RK_FORMAT_RGB_888;
				double q0 = now_ms();
				IM_STATUS st = imresize(s, d);
				double q1 = now_ms();
				/* uint8 -> int8(zp=-128):RGA2 没有 NN 单元,只能做一遍偏置。
				 * 关键优化:"减 128" 在字节上等价于 "xor 0x80",
				 * 所以可以 8 字节一次位运算做完,而不是逐字节 memmove。 */
				{
					uint64_t *t64 = (uint64_t *)in_mem->virt_addr;
					size_t n64 = (size_t)OUT_W * OUT_H * 3 / 8;
					const uint64_t mask = 0x8080808080808080ULL;
					for (size_t k = 0; k < n64; k++) t64[k] ^= mask;
				}
				double q2 = now_ms();
				static double acc_rga = 0, acc_bias = 0;
				acc_rga += q1 - q0; acc_bias += q2 - q1;
				if (f == frames - 1)
					printf("mode C 细分: RGA %.2f ms/帧  int8偏置 %.2f ms/帧  (合计 %.2f)\n",
					       acc_rga / frames, acc_bias / frames, (acc_rga + acc_bias) / frames);
				if (f == 0) printf("mode C: rga fd->fd = %s\n", imStrError(st));
			}
		}
		double t2 = now_ms();
		t_pre += t2 - t1;

		/* ---- 推理 ---- */
		if (mode != 'c') {
			rknn_input in; memset(&in, 0, sizeof in);
			in.index = 0; in.type = RKNN_TENSOR_UINT8; in.fmt = RKNN_TENSOR_NHWC;
			in.size = (uint32_t)(OUT_W * OUT_H * 3); in.buf = host_rgb;
			ret = rknn_inputs_set(ctx, 1, &in);     /* 内部会再拷 1.2MB */
			if (ret < 0) { printf("inputs_set fail %d\n", ret); break; }
			g_copied += (unsigned long long)OUT_W * OUT_H * 3; g_copy_calls++;
		}
		double t3 = now_ms();
		{
			const char *env = getenv("RUNS");
			int runs = env ? atoi(env) : 1;
			for (int r = 0; r < runs; r++) {
				ret = rknn_run(ctx, NULL);
				if (ret < 0) { printf("rknn_run fail %d\n", ret); break; }
			}
		}
		double t4 = now_ms();
		t_run += t4 - t3;

		/* ---- 取输出 + 解码 ---- */
		std::vector<box> boxes;
		if (mode == 'c') {
			int8_t *ptrs[32];
			for (unsigned i = 0; i < nout; i++) ptrs[i] = (int8_t *)out_mems[i]->virt_addr;
			decode_yolov8(oa, ptrs, boxes);
		} else {
			rknn_output *outs = (rknn_output *)calloc(nout, sizeof(rknn_output));
			for (unsigned i = 0; i < nout; i++) { outs[i].index = i; outs[i].want_float = 0; }
			ret = rknn_outputs_get(ctx, nout, outs, NULL);
			if (ret < 0) { printf("outputs_get fail %d\n", ret); break; }
			int8_t *ptrs[32];
			for (unsigned i = 0; i < nout; i++) ptrs[i] = (int8_t *)outs[i].buf;
			decode_yolov8(oa, ptrs, boxes);
			rknn_outputs_release(ctx, nout, outs);
			free(outs);
		}
		double t5 = now_ms();
		t_post += t5 - t4;
		total_boxes += (int)boxes.size();

		if (f == frames - 1 && !boxes.empty()) {
			/* 画框:模式 C 用输入 tensor 的内存直接画 */
			uint8_t *canvas = host_rgb;
			if (mode == 'c') {
				canvas = (uint8_t *)calloc(1, (size_t)OUT_W * OUT_H * 3);
				const int8_t *src = (const int8_t *)in_mem->virt_addr;
				for (size_t i = 0; i < (size_t)OUT_W * OUT_H * 3; i++)
					canvas[i] = (uint8_t)(src[i] + 128);
			}
			draw_boxes(canvas, OUT_W, OUT_H, boxes);
			FILE *pf = fopen("/root/s4_det.ppm", "wb");
			fprintf(pf, "P6\n%d %d\n255\n", OUT_W, OUT_H);
			fwrite(canvas, 1, (size_t)OUT_W * OUT_H * 3, pf);
			fclose(pf);
			printf("最后一帧画出 %zu 个框 -> /root/s4_det.ppm\n", boxes.size());
			if (mode == 'c') free(canvas);
		}

		xioctl(fd, VIDIOC_QBUF, &b);
	}

	xioctl(fd, VIDIOC_STREAMOFF, &btype);

	getrusage(RUSAGE_SELF, &ru1);
	double wall1 = now_ms();
	double n = frames;
	double cpu_ms = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) * 1000.0
	              + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1000.0
	              + (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) * 1000.0
	              + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1000.0;
	printf("\n===== mode %c 统计(%d 帧) =====\n", mode, frames);
	printf("CPU 时间  %.2f ms/帧   (占墙钟 %.1f%%)\n", cpu_ms / n, 100.0 * cpu_ms / (wall1 - wall0));
	printf("capture   %.2f ms/帧\n", t_cap / n);
	printf("preproc   %.2f ms/帧\n", t_pre / n);
	printf("rknn_run  %.2f ms/帧\n", t_run / n);
	printf("postproc  %.2f ms/帧\n", t_post / n);
	printf("总拷贝    %llu 字节/帧 (%.2f MB, %llu 次 memcpy)\n",
	       g_copied / (unsigned long long)frames,
	       g_copied / (double)frames / 1048576.0,
	       g_copy_calls / (unsigned long long)frames);
	printf("检测框总数 %d (最后一帧见 /root/s4_det.ppm)\n", total_boxes);

	return 0;
}
