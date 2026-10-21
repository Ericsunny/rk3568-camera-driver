/*
 * zc_dual.cpp — 双路同时接 NPU,测争用
 *
 *   通道 A:IMX415 @ rkisp  NV12 3840x2160  -> EXPBUF dmabuf -> RGA -> 640x640 RGB -> NPU
 *   通道 B:OV8858 @ rkcif  RAW10 1632x1224  -> memcpy -> 解包成灰度 -> RGA(YCbCr_400->RGB) -> NPU
 *
 * 两路各用独立 rknn context(与流水线版同思路),各自统计 fps / 每帧延迟 / CPU。
 * 目的:看一颗 NPU 在两路 45 fps 的需求下(15 + 30)如何分配,聚合吞吐是多少。
 *
 * 编译:
 *   g++ -O2 -o zc_dual zc_dual.cpp -I/usr/include/rga \
 *       -I/root/npu/3rdparty/rknpu2/include -lrga -lrknnrt -lpthread
 * 用法:./zc_dual <秒>
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <linux/videodev2.h>
#include <time.h>

#include <rga/im2d.h>
#include <rga/rga.h>
#include "rknn_api.h"

#define NBUF    4
#define OUT_W   640
#define OUT_H   640
#define MODEL   "/root/npu/examples/yolov8/model/yolov8n_rk3568_i8.rknn"

static double now_ms(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}

static rknn_tensor_attr g_oa[16];
static unsigned g_nout;

struct box { float x1, y1, x2, y2, score; int cls; };

static int decode(rknn_tensor_attr *oa, int8_t **od, std::vector<box> &res)
{
	static const int strides[3] = {8, 16, 32};
	for (int s = 0; s < 3; s++) {
		rknn_tensor_attr &areg = oa[s*3+0], &acls = oa[s*3+1], &ascr = oa[s*3+2];
		int gh = areg.dims[2], gw = areg.dims[3], stride = strides[s];
		int8_t *reg = od[s*3+0], *cls = od[s*3+1], *scr = od[s*3+2];
		for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) {
			int idx = y * gw + x;
			float score = (scr[idx] - ascr.zp) * ascr.scale;
			if (score < 0.35f) continue;
			int best = -1; float bestv = 0;
			for (int c = 0; c < 80; c++) {
				float v = (cls[c * gh * gw + idx] - acls.zp) * acls.scale;
				if (v > bestv) { bestv = v; best = c; }
			}
			if (best < 0 || score * bestv < 0.35f) continue;
			float d[4];
			for (int side = 0; side < 4; side++) {
				float p[16], mx = -1e9f, sum = 0;
				for (int k = 0; k < 16; k++) {
					float v = (reg[(side*16+k) * gh * gw + idx] - areg.zp) * areg.scale;
					p[k] = v; if (v > mx) mx = v;
				}
				for (int k = 0; k < 16; k++) { p[k] = expf(p[k] - mx); sum += p[k]; }
				float acc = 0;
				for (int k = 0; k < 16; k++) acc += (p[k] / sum) * k;
				d[side] = acc;
			}
			float cx = (x + 0.5f) * stride, cy = (y + 0.5f) * stride;
			box b;
			b.x1 = cx - d[0]*stride; b.y1 = cy - d[1]*stride;
			b.x2 = cx + d[2]*stride; b.y2 = cy + d[3]*stride;
			b.score = score * bestv; b.cls = best;
			res.push_back(b);
		}
	}
	return (int)res.size();
}

/* ---------------- 通道 ---------------- */
struct chan {
	const char *name;
	const char *dev;
	int is_raw10;              /* 1 = OV8858 RAW10, 0 = NV12 */
	int w, h, bpl;
	rknn_context ctx;
	rknn_tensor_mem *in_mem;
	rknn_tensor_mem *out_mems[16];
	int fd, nplanes;
	void *mem[NBUF]; size_t len[NBUF]; int expfd[NBUF];
	unsigned nbufs;
	uint8_t *gray;             /* RAW10 解包目标 */
	int frames;                /* 完成帧数 */
	double lat_ms;             /* 累计延迟 */
	double t_npu_ms;           /* 累计推理时间 */
	int infer_ok;
};

static volatile int g_stop = 0;
static double g_deadline;

static void *chan_thread(void *arg)
{
	chan *c = (chan *)arg;
	while (!g_stop) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.m.planes = p; b.length = c->nplanes;
		if (xioctl(c->fd, VIDIOC_DQBUF, &b) < 0) break;
		double t0 = now_ms();
		if (t0 > g_deadline) { xioctl(c->fd, VIDIOC_QBUF, &b); g_stop = 1; break; }

		int idx = b.index;
		rga_buffer_t s, d;
		memset(&s, 0, sizeof s); memset(&d, 0, sizeof d);

		if (!c->is_raw10) {
			/* A:dmabuf fd 直接喂 RGA */
			s.fd = c->expfd[idx]; s.width = c->w; s.height = c->h;
			s.wstride = c->w; s.hstride = c->h; s.format = RK_FORMAT_YCbCr_420_SP;
		} else {
			/* B:先 memcpy 到可缓存内存,再解包成灰度(5 字节取前 4 字节即高 8 位) */
			memcpy(c->gray + 0, c->mem[idx], 0);   /* 占位,下面按行做 */
			uint8_t *tmp = (uint8_t *)malloc(c->len[idx]);
			memcpy(tmp, c->mem[idx], c->len[idx]);
			for (int y = 0; y < c->h; y++) {
				const uint8_t *sp = tmp + (size_t)y * c->bpl;
				uint8_t *dp = c->gray + (size_t)y * c->w;
				int groups = c->w / 4;
				for (int i = 0; i < groups; i++) {
					uint32_t v;
					memcpy(&v, sp + i * 5, 4);
					memcpy(dp + i * 4, &v, 4);
				}
			}
			free(tmp);
			s.vir_addr = c->gray; s.width = c->w; s.height = c->h;
			s.wstride = c->w; s.hstride = c->h; s.format = RK_FORMAT_YCbCr_400;
		}
		d.fd = c->in_mem->fd; d.width = OUT_W; d.height = OUT_H;
		d.wstride = OUT_W; d.hstride = OUT_H; d.format = RK_FORMAT_RGB_888;
		imresize(s, d);

		/* uint8 -> int8(zp=-128) = 8 字节异或 */
		uint64_t *t64 = (uint64_t *)c->in_mem->virt_addr;
		size_t n64 = (size_t)OUT_W * OUT_H * 3 / 8;
		const uint64_t mask = 0x8080808080808080ULL;
		for (size_t k = 0; k < n64; k++) t64[k] ^= mask;

		double tn0 = now_ms();
		int ret = rknn_run(c->ctx, NULL);
		double tn1 = now_ms();
		if (ret == 0) c->infer_ok++;

		std::vector<box> boxes;
		int8_t *ptrs[16];
		for (unsigned i = 0; i < g_nout; i++) ptrs[i] = (int8_t *)c->out_mems[i]->virt_addr;
		decode(g_oa, ptrs, boxes);

		xioctl(c->fd, VIDIOC_QBUF, &b);
		c->frames++;
		c->lat_ms += (tn1 - t0);
		c->t_npu_ms += (tn1 - tn0);
	}
	return NULL;
}

static int chan_init(chan *c, const char *name, const char *dev, int raw10,
		     int w, int h, uint32_t pf, void *model, long msz)
{
	memset(c, 0, sizeof *c);
	c->name = name; c->dev = dev; c->is_raw10 = raw10; c->w = w; c->h = h;

	/* rknn */
	rknn_input_output_num io_num;
	if (rknn_init(&c->ctx, model, msz, 0, NULL) < 0) { printf("%s: rknn_init fail\n", name); return -1; }
	rknn_query(c->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof io_num);
	g_nout = io_num.n_output;
	rknn_tensor_attr in; memset(&in, 0, sizeof in); in.index = 0;
	rknn_query(c->ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &in, sizeof in);
	in.pass_through = 1;
	c->in_mem = rknn_create_mem(c->ctx, in.size_with_stride);
	rknn_set_io_mem(c->ctx, c->in_mem, &in);
	for (unsigned i = 0; i < g_nout; i++) {
		memset(&g_oa[i], 0, sizeof g_oa[i]); g_oa[i].index = i;
		rknn_query(c->ctx, RKNN_QUERY_OUTPUT_ATTR, &g_oa[i], sizeof g_oa[i]);
		c->out_mems[i] = rknn_create_mem(c->ctx, g_oa[i].size_with_stride);
		rknn_set_io_mem(c->ctx, c->out_mems[i], &g_oa[i]);
	}

	/* v4l2 */
	c->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (c->fd < 0) { perror(dev); return -1; }
	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return -1; }
	fmt.fmt.pix_mp.width = w; fmt.fmt.pix_mp.height = h; fmt.fmt.pix_mp.pixelformat = pf;
	if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
	c->w = fmt.fmt.pix_mp.width; c->h = fmt.fmt.pix_mp.height;
	c->nplanes = fmt.fmt.pix_mp.num_planes;
	c->bpl = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

	struct v4l2_requestbuffers req; memset(&req, 0, sizeof req);
	req.count = NBUF; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }
	c->nbufs = req.count;

	for (unsigned i = 0; i < c->nbufs; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = c->nplanes;
		xioctl(c->fd, VIDIOC_QUERYBUF, &b);
		c->len[i] = p[0].length;
		c->mem[i] = mmap(NULL, c->len[i], PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, p[0].m.mem_offset);
		struct v4l2_exportbuffer e; memset(&e, 0, sizeof e);
		e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; e.index = i; e.plane = 0;
		e.flags = O_RDWR | O_CLOEXEC;
		c->expfd[i] = (xioctl(c->fd, VIDIOC_EXPBUF, &e) == 0) ? e.fd : -1;
	}
	if (raw10) c->gray = (uint8_t *)malloc((size_t)c->w * c->h);

	for (unsigned i = 0; i < c->nbufs; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = c->nplanes;
		xioctl(c->fd, VIDIOC_QBUF, &b);
	}
	enum v4l2_buf_type bt = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(c->fd, VIDIOC_STREAMON, &bt) < 0) { perror("STREAMON"); return -1; }

	printf("%-22s %dx%d %s  expbuf=%d\n", name, c->w, c->h,
	       raw10 ? "RAW10" : "NV12", c->expfd[0]);
	return 0;
}

int main(int argc, char **argv)
{
	double secs = (argc > 1) ? atof(argv[1]) : 10.0;

	FILE *mf = fopen(MODEL, "rb");
	if (!mf) { perror(MODEL); return 1; }
	fseek(mf, 0, SEEK_END); long msz = ftell(mf); fseek(mf, 0, SEEK_SET);
	void *model = malloc(msz);
	if (fread(model, 1, msz, mf) != (size_t)msz) return 1;
	fclose(mf);

	static chan A, B;
	/* 只跑单路的对照:参数 r 时只开 A */
	int only_a = (argc > 2 && argv[2][0] == 'a');
	int only_b = (argc > 2 && argv[2][0] == 'b');

	if (!only_b && chan_init(&A, "IMX415 @ rkisp", "/dev/video5", 0, 3840, 2160,
				 V4L2_PIX_FMT_NV12, model, msz) < 0) return 1;
	if (!only_a && chan_init(&B, "OV8858 @ rkcif", "/dev/video0", 1, 1632, 1224,
				 V4L2_PIX_FMT_SBGGR10, model, msz) < 0) return 1;

	struct rusage r0, r1;
	getrusage(RUSAGE_SELF, &r0);
	double w0 = now_ms();
	g_deadline = w0 + secs * 1000.0;

	pthread_t ta, tb;
	int has_a = !only_b, has_b = !only_a;
	if (has_a) pthread_create(&ta, NULL, chan_thread, &A);
	if (has_b) pthread_create(&tb, NULL, chan_thread, &B);
	if (has_a) pthread_join(ta, NULL);
	if (has_b) pthread_join(tb, NULL);

	double w1 = now_ms();
	getrusage(RUSAGE_SELF, &r1);
	double wall = (w1 - w0) / 1000.0;
	double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec)
	           + (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec)
	           + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1e6
	           + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;

	printf("\n===== %.1f 秒实测 =====\n", wall);
	int total = 0;
	if (has_a) {
		printf("%-22s %4d 帧  %6.2f fps  每帧 %.1f ms  NPU %.1f ms/帧\n",
		       A.name, A.frames, A.frames / wall, A.lat_ms / (A.frames ? A.frames : 1),
		       A.t_npu_ms / (A.frames ? A.frames : 1));
		total += A.frames;
	}
	if (has_b) {
		printf("%-22s %4d 帧  %6.2f fps  每帧 %.1f ms  NPU %.1f ms/帧\n",
		       B.name, B.frames, B.frames / wall, B.lat_ms / (B.frames ? B.frames : 1),
		       B.t_npu_ms / (B.frames ? B.frames : 1));
		total += B.frames;
	}
	printf("聚合           : %d 帧 / %.2fs = %.2f inf/s\n", total, wall, total / wall);
	printf("进程 CPU       : %.2f s / %.2f s = %.1f%%(4 核 %.2f 核)\n",
	       cpu, wall, 100.0 * cpu / wall, cpu / wall);
	return 0;
}
