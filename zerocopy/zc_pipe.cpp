/*
 * zc_pipe.cpp — 流水线版零拷贝:采集 / 前处理(RGA+int8) / 推理(NPU) 三线程重叠
 *
 * 串行版(mode C)每帧 69.9 ms = 采集 5 + 前处理 23.5 + 推理 40.9 + 后处理 0.4
 * 流水线目标:吞吐 ≈ max(23.5, 40.9) ≈ 41 ms/帧
 *
 * 设计:
 *   3 个槽位,每个槽位一套 rknn context + 输入/输出 tensor 内存(io_mem 绑定互不干扰)
 *   cap 线程:DQBUF -> 抢空槽 -> 交给 pre
 *   pre 线程:EXPBUF(已预导出) -> RGA 缩放 -> 8 字节异或补 int8 -> 交给 inf
 *   inf 线程:rknn_run -> 解码 -> QBUF 还回 V4L2 -> 槽位释放
 *
 * 编译:
 *   g++ -O2 -o zc_pipe zc_pipe.cpp -I/usr/include/rga \
 *       -I/root/npu/3rdparty/rknpu2/include -lrga -lrknnrt -lpthread
 * 运行:./zc_pipe [frames] [device]
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

#define NBUF     4
#define NSLOT    3
#define OUT_W    640
#define OUT_H    640
#define MODEL    "/root/npu/examples/yolov8/model/yolov8n_rk3568_i8.rknn"

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

/* ---------------- 模型 / 输出属性 ---------------- */
static rknn_tensor_attr g_oa[16];
static unsigned g_nout;

struct box { float x1, y1, x2, y2, score; int cls; };

static void decode(rknn_tensor_attr *oa, int8_t **od, std::vector<box> &res)
{
	static const int strides[3] = {8, 16, 32};
	for (int s = 0; s < 3; s++) {
		rknn_tensor_attr &areg = oa[s * 3 + 0], &acls = oa[s * 3 + 1], &ascr = oa[s * 3 + 2];
		int gh = areg.dims[2], gw = areg.dims[3], stride = strides[s];
		int8_t *reg = od[s * 3 + 0], *cls = od[s * 3 + 1], *scr = od[s * 3 + 2];
		for (int y = 0; y < gh; y++) {
			for (int x = 0; x < gw; x++) {
				int idx = y * gw + x;
				float score = (scr[idx] - ascr.zp) * ascr.scale;
				if (score < 0.35f) continue;
				int best = -1; float bestv = 0;
				for (int c = 0; c < 80; c++) {
					float v = (cls[c * gh * gw + idx] - acls.zp) * acls.scale;
					if (v > bestv) { bestv = v; best = c; }
				}
				if (best < 0 || score * bestv < 0.35f) continue;
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
				b.score = score * bestv; b.cls = best;
				res.push_back(b);
			}
		}
	}
}

/* ---------------- 槽位 ---------------- */
struct slot {
	rknn_context ctx;
	rknn_tensor_mem *in_mem;
	rknn_tensor_mem *out_mems[16];
	int state;          /* 0=空 1=已采集 2=已前处理(待推理) */
	int v4l2_idx;
	double t_cap, t_pre, t_inf;
	int boxes;
};

static slot g_slot[NSLOT];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_stop = 0;
static int g_frames = 0, g_done = 0, g_runs = 1;

/* 共享采集状态 */
static int g_vfd, g_sw, g_sh, g_nplanes;
static void *g_mem[NBUF]; static size_t g_len[NBUF]; static int g_expfd[NBUF];
static unsigned g_nbufs;
static rknn_tensor_attr g_in_native;

static double g_t_first_cap, g_t_last_done;
static int g_pre_count = 0, g_inf_count = 0;

/* ---------------- 线程 ---------------- */
static void *cap_thread(void *arg)
{
	(void)arg;
	int produced = 0;
	while (produced < g_frames) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.m.planes = p; b.length = g_nplanes;
		if (xioctl(g_vfd, VIDIOC_DQBUF, &b) < 0) break;
		double tc = now_ms();
		if (produced == 0) g_t_first_cap = tc;

		pthread_mutex_lock(&g_mu);
		int s;
		while ((s = -1), 1) {
			for (int i = 0; i < NSLOT; i++) if (g_slot[i].state == 0) { s = i; break; }
			if (s >= 0) break;
			pthread_cond_wait(&g_cv, &g_mu);
		}
		g_slot[s].v4l2_idx = b.index;
		g_slot[s].t_cap = tc;
		g_slot[s].state = 1;
		pthread_cond_broadcast(&g_cv);
		pthread_mutex_unlock(&g_mu);
		produced++;
	}
	return NULL;
}

static void *pre_thread(void *arg)
{
	(void)arg;
	while (!g_stop) {
		pthread_mutex_lock(&g_mu);
		int s = -1;
		while (!g_stop) {
			for (int i = 0; i < NSLOT; i++) if (g_slot[i].state == 1) { s = i; break; }
			if (s >= 0) break;
			pthread_cond_wait(&g_cv, &g_mu);
		}
		if (s < 0) { pthread_mutex_unlock(&g_mu); break; }
		slot &sl = g_slot[s];
		int vidx = sl.v4l2_idx, efd = g_expfd[vidx];
		pthread_mutex_unlock(&g_mu);

		double t0 = now_ms();
		rga_buffer_t src; memset(&src, 0, sizeof src);
		src.fd = efd; src.width = g_sw; src.height = g_sh;
		src.wstride = g_sw; src.hstride = g_sh; src.format = RK_FORMAT_YCbCr_420_SP;
		rga_buffer_t dst; memset(&dst, 0, sizeof dst);
		dst.fd = sl.in_mem->fd; dst.width = OUT_W; dst.height = OUT_H;
		dst.wstride = OUT_W; dst.hstride = OUT_H; dst.format = RK_FORMAT_RGB_888;
		imresize(src, dst);
		/* uint8 -> int8(zp=-128):等价于 8 字节异或 0x80 */
		uint64_t *t64 = (uint64_t *)sl.in_mem->virt_addr;
		size_t n64 = (size_t)OUT_W * OUT_H * 3 / 8;
		const uint64_t mask = 0x8080808080808080ULL;
		for (size_t k = 0; k < n64; k++) t64[k] ^= mask;
		double t1 = now_ms();

		pthread_mutex_lock(&g_mu);
		sl.t_pre = t1 - t0;
		sl.state = 2;
		g_pre_count++;
		pthread_cond_broadcast(&g_cv);
		pthread_mutex_unlock(&g_mu);
	}
	return NULL;
}

static void *inf_thread(void *arg)
{
	(void)arg;
	while (!g_stop) {
		pthread_mutex_lock(&g_mu);
		int s = -1;
		while (!g_stop) {
			for (int i = 0; i < NSLOT; i++) if (g_slot[i].state == 2) { s = i; break; }
			if (s >= 0) break;
			pthread_cond_wait(&g_cv, &g_mu);
		}
		if (s < 0) { pthread_mutex_unlock(&g_mu); break; }
		slot &sl = g_slot[s];
		pthread_mutex_unlock(&g_mu);

		double t0 = now_ms();
		for (int r = 0; r < g_runs; r++) rknn_run(sl.ctx, NULL);
		std::vector<box> boxes;
		int8_t *ptrs[16];
		for (unsigned i = 0; i < g_nout; i++) ptrs[i] = (int8_t *)sl.out_mems[i]->virt_addr;
		decode(g_oa, ptrs, boxes);
		double t1 = now_ms();

		/* 还回 V4L2 */
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = sl.v4l2_idx; b.m.planes = p; b.length = g_nplanes;
		xioctl(g_vfd, VIDIOC_QBUF, &b);
		double t2 = now_ms();

		pthread_mutex_lock(&g_mu);
		sl.t_inf = t1 - t0;
		sl.boxes = (int)boxes.size();
		sl.state = 0;
		g_done++;
		g_t_last_done = t2;
		if (g_done >= g_frames) g_stop = 1;
		pthread_cond_broadcast(&g_cv);
		pthread_mutex_unlock(&g_mu);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	g_frames = (argc > 1) ? atoi(argv[1]) : 60;
	const char *dev = (argc > 2) ? argv[2] : "/dev/video5";
	g_runs = (argc > 3) ? atoi(argv[3]) : 1;

	/* 模型 */
	FILE *mf = fopen(MODEL, "rb");
	if (!mf) { perror("model"); return 1; }
	fseek(mf, 0, SEEK_END); long msz = ftell(mf); fseek(mf, 0, SEEK_SET);
	void *model = malloc(msz);
	if (fread(model, 1, msz, mf) != (size_t)msz) return 1;
	fclose(mf);

	/* 每槽一个 context */
	rknn_input_output_num io_num;
	for (int s = 0; s < NSLOT; s++) {
		if (rknn_init(&g_slot[s].ctx, model, msz, 0, NULL) < 0) { printf("init fail\n"); return 1; }
		rknn_query(g_slot[s].ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof io_num);
		g_nout = io_num.n_output;
		memset(&g_in_native, 0, sizeof g_in_native); g_in_native.index = 0;
		rknn_query(g_slot[s].ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &g_in_native, sizeof g_in_native);
		g_in_native.pass_through = 1;
		g_slot[s].in_mem = rknn_create_mem(g_slot[s].ctx, g_in_native.size_with_stride);
		rknn_set_io_mem(g_slot[s].ctx, g_slot[s].in_mem, &g_in_native);
		for (unsigned i = 0; i < g_nout; i++) {
			memset(&g_oa[i], 0, sizeof g_oa[i]); g_oa[i].index = i;
			rknn_query(g_slot[s].ctx, RKNN_QUERY_OUTPUT_ATTR, &g_oa[i], sizeof g_oa[i]);
			g_slot[s].out_mems[i] = rknn_create_mem(g_slot[s].ctx, g_oa[i].size_with_stride);
			rknn_set_io_mem(g_slot[s].ctx, g_slot[s].out_mems[i], &g_oa[i]);
		}
		g_slot[s].state = 0;
	}
	printf("初始化 %d 个槽位(每个独立 rknn context),输出 %u 个\n", NSLOT, g_nout);

	/* 采集 */
	g_vfd = open(dev, O_RDWR | O_CLOEXEC);
	if (g_vfd < 0) { perror("open video"); return 1; }
	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(g_vfd, VIDIOC_G_FMT, &fmt);
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	xioctl(g_vfd, VIDIOC_S_FMT, &fmt);
	g_sw = fmt.fmt.pix_mp.width; g_sh = fmt.fmt.pix_mp.height; g_nplanes = fmt.fmt.pix_mp.num_planes;

	struct v4l2_requestbuffers req; memset(&req, 0, sizeof req);
	req.count = NBUF; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
	xioctl(g_vfd, VIDIOC_REQBUFS, &req);
	g_nbufs = req.count;

	for (unsigned i = 0; i < g_nbufs; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = g_nplanes;
		xioctl(g_vfd, VIDIOC_QUERYBUF, &b);
		g_len[i] = p[0].length;
		g_mem[i] = mmap(NULL, g_len[i], PROT_READ | PROT_WRITE, MAP_SHARED, g_vfd, p[0].m.mem_offset);
		struct v4l2_exportbuffer e; memset(&e, 0, sizeof e);
		e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; e.index = i; e.plane = 0;
		e.flags = O_RDWR | O_CLOEXEC;
		g_expfd[i] = (xioctl(g_vfd, VIDIOC_EXPBUF, &e) == 0) ? e.fd : -1;
	}
	for (unsigned i = 0; i < g_nbufs; i++) {
		struct v4l2_buffer b; struct v4l2_plane p[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof b); memset(p, 0, sizeof p);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = p; b.length = g_nplanes;
		xioctl(g_vfd, VIDIOC_QBUF, &b);
	}
	enum v4l2_buf_type bt = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(g_vfd, VIDIOC_STREAMON, &bt);
	printf("采集 %dx%d NV12, %u buffer, 目标 %d 帧, 每帧推理 %d 次\n", g_sw, g_sh, g_nbufs, g_frames, g_runs);

	struct rusage r0, r1;
	getrusage(RUSAGE_SELF, &r0);
	double wall0 = now_ms();

	pthread_t t1, t2, t3;
	pthread_create(&t1, NULL, cap_thread, NULL);
	pthread_create(&t2, NULL, pre_thread, NULL);
	pthread_create(&t3, NULL, inf_thread, NULL);
	pthread_join(t1, NULL);
	pthread_join(t3, NULL);
	g_stop = 1;
	pthread_cond_broadcast(&g_cv);
	pthread_join(t2, NULL);

	double wall1 = now_ms();
	getrusage(RUSAGE_SELF, &r1);
	xioctl(g_vfd, VIDIOC_STREAMOFF, &bt);

	double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) * 1000.0
	           + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1000.0
	           + (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1000.0
	           + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1000.0;

	double wall = wall1 - wall0;
	printf("\n===== 流水线(3 线程 / 3 槽位)=====\n");
	printf("完成帧数   : %d / %d\n", g_done, g_frames);
	printf("总墙钟     : %.1f ms  -> %.2f fps (%.2f ms/帧)\n", wall, g_done * 1000.0 / wall, wall / g_done);
	printf("端到端延迟 : 首帧采集 -> 末帧完成 = %.1f ms (含流水填充)\n", g_t_last_done - g_t_first_cap);
	printf("CPU 时间   : %.2f ms/帧  (占墙钟 %.1f%%)\n", cpu / g_done, 100.0 * cpu / wall);

	/* 各阶段平均 */
	double tp = 0, ti = 0;
	for (int i = 0; i < NSLOT; i++) { tp += g_slot[i].t_pre; ti += g_slot[i].t_inf; }
	printf("(槽位样本)前处理 %.2f ms  推理 %.2f ms\n", tp / NSLOT, ti / NSLOT);

	xioctl(g_vfd, VIDIOC_STREAMOFF, &bt);
	for (unsigned i = 0; i < g_nbufs; i++) {
		if (g_expfd[i] >= 0) close(g_expfd[i]);
		munmap(g_mem[i], g_len[i]);
	}
	close(g_vfd);
	return 0;
}
