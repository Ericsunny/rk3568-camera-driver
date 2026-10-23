// camd.c —— RK3568 相机服务:双消费者 + 有界队列 + buffer 生命周期 + 可诊断退出
//
// 设计要点(docs/29):
//   1. 一个 V4L2 buffer 只有一个归属链:capture 持有一份,分发给每个消费者各加
//      一份引用;全部 release 后 refcount 归零才 QBUF 还给内核。DMA-BUF fd 共享
//      出去 ≠ 访问已完成,归还时机由引用计数决定,不由"分析耗了多久"决定。
//   2. 两条队列策略不同:实时分支队列满丢最旧(保新帧);取证分支限速 + 有界阻塞
//      (背压),超时丢并计数——任何情况下都不无限堆内存。
//   3. 停止靠 pthread mutex/cond + 原子标志驱动,不用 volatile 冒充同步。
//   4. 故障只做一次有界恢复:恢复失败就干净退出 + 诊断包,不无限重启。
//
// 板上编译: gcc -O2 -pthread -o camd camd.c -ldl

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>

#include <linux/videodev2.h>

#ifdef HAVE_RGA
#include "im2d.h"		/* librga 1.9.x:C 接口 *_t 版本 */
#endif
#ifdef HAVE_RKNN
#include "rknn_api.h"
#endif

#define MAX_BUFS 8
#define MAX_Q 32

/* ---------------- 时间 ---------------- */
static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---------------- 延迟直方(定长环形,算分位数) ---------------- */
typedef struct {
	uint64_t n, sum_us, max_us;
	int cap, head;
	uint32_t *ring;
} lat_t;

static void lat_init(lat_t *l, int cap)
{
	memset(l, 0, sizeof(*l));
	l->cap = cap;
	l->ring = cap ? calloc(cap, sizeof(uint32_t)) : NULL;
}

static void lat_reset(lat_t *l)
{
	l->n = l->sum_us = l->max_us = 0;
	l->head = 0;
	if (l->ring)
		memset(l->ring, 0, sizeof(uint32_t) * l->cap);
}

static void lat_add(lat_t *l, int64_t us)
{
	if (us < 0)
		us = 0;
	l->n++;
	l->sum_us += us;
	if ((uint64_t)us > l->max_us)
		l->max_us = us;
	if (l->ring) {
		l->ring[l->head] = (uint32_t)us;
		l->head = (l->head + 1) % l->cap;
	}
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

static double lat_pct(lat_t *l, double p)
{
	uint32_t *tmp;
	int k;

	if (!l->cap || l->n == 0)
		return -1;
	tmp = malloc(sizeof(uint32_t) * l->cap);
	if (!tmp)
		return -1;
	memcpy(tmp, l->ring, sizeof(uint32_t) * l->cap);
	qsort(tmp, l->cap, sizeof(uint32_t), cmp_u32);
	k = (int)(p * (l->cap - 1) + 0.5);
	if (k < 0)
		k = 0;
	if (k >= l->cap)
		k = l->cap - 1;
	k = tmp[k];
	free(tmp);
	return k;
}

/* ---------------- 帧节点 ---------------- */
struct node {
	int idx;
	size_t len;
	int dmabuf_fd;
	int stride;		/* 行 stride(bytesperline),RGA 按它解释内存 */
	uint64_t seq;
	int64_t ts_us;
	atomic_int refs;
};

typedef struct {
	struct node *slot[MAX_Q];
	int cap, n, head;
	int drop_oldest;	/* 1 = 丢最旧;0 = 有界阻塞 */
	int block_ms;
	pthread_mutex_t m;
	pthread_cond_t not_empty, not_full;
	uint64_t pushed, popped, dropped;
} queue_t;

/* ---------------- 全局状态 ---------------- */
static struct {
	int fd;
	int w, h, nbufs;
	uint32_t fourcc;
	struct node *nodes[MAX_BUFS];
	void *mmap[MAX_BUFS];	/* CPU 对照路径需要可读基址 */
	atomic_bool streaming;
} cap;

static queue_t q[2];
enum { Q_RT = 0, Q_EV = 1 };

static atomic_bool g_run;
static atomic_bool g_draining;
static atomic_int g_fatal;
static char g_fatal_msg[256];
static atomic_uint g_frames, g_frames_done, g_qbuf_err;
static int64_t g_first_us, g_start_us, g_last_us;
static pthread_mutex_t g_first_mu = PTHREAD_MUTEX_INITIALIZER;

static lat_t rt_lat, ev_lat;

/* ---------------- RGA 零拷贝消费者 ---------------- */
static int g_rga;			/* --rga 打开 */
static int g_copy;			/* --cpu-copy:CPU 拷贝路径(对照组) */
#ifdef HAVE_RGA
static unsigned char *g_copybuf;	/* CPU 侧全帧副本 */
#endif

static int g_rga_dw = 640, g_rga_dh = 360;
#ifdef HAVE_RGA
static rga_buffer_t g_rga_dst;
static unsigned char *g_rga_dstbuf;
static atomic_uint g_rga_ok, g_rga_err;
static char g_rga_msg[128];
static lat_t rga_lat;

static int rga_init_dst(void)
{
	size_t need = (size_t)g_rga_dw * g_rga_dh * 3 / 2;

	g_rga_dstbuf = malloc(need);
	if (!g_rga_dstbuf)
		return -1;
	memset(g_rga_dstbuf, 0, need);
	g_rga_dst = wrapbuffer_virtualaddr_t(g_rga_dstbuf, g_rga_dw, g_rga_dh,
					     g_rga_dw, g_rga_dh,
					     RK_FORMAT_YCbCr_420_SP);
	lat_init(&rga_lat, 2048);
	return 0;
}

/* 对照组:CPU 把整帧 memcpy 出来,再做 4x4 盒式降采样(与 RGA 同输出尺寸) */
static void cpu_copy_process(struct node *n)
{
	size_t need = n->len;
	unsigned char *dst = g_rga_dstbuf;
	int x, y, i, j, dw = g_rga_dw, dh = g_rga_dh;
	int stride = n->stride > 0 ? n->stride : cap.w;
	int bx = cap.w / dw, by = cap.h / dh;
	volatile unsigned long sum = 0;

	if (!g_copybuf) {
		g_copybuf = malloc(need);
		if (!g_copybuf)
			return;
	}
	/* 真实基址来自 mmap(在 dev_open 里保存),见 cap.mmap[i] */
	memcpy(g_copybuf, cap.mmap[n->idx], need);
	for (y = 0; y < dh; y++) {
		for (x = 0; x < dw; x++) {
			int acc = 0;
			for (j = 0; j < by; j++) {
				const unsigned char *row = g_copybuf +
					(size_t)(y * by + j) * stride + x * bx;
				for (i = 0; i < bx; i++)
					acc += row[i];
			}
			dst[(size_t)y * dw + x] = (unsigned char)(acc / (bx * by));
		}
	}
	sum += dst[0];
	(void)sum;
}

/* 从 DMA-BUF fd 直接给 RGA,不经 CPU 拷贝;NV12 走 420SP,其余按 8bit 灰度搬 */
static void rga_process(struct node *n)
{
	int fmt = (cap.fourcc == V4L2_PIX_FMT_NV12) ? RK_FORMAT_YCbCr_420_SP
						    : RK_FORMAT_YCbCr_400;
	int64_t t0 = now_us();
	rga_buffer_t src;
	IM_STATUS st;

	src = wrapbuffer_fd_t(n->dmabuf_fd, cap.w, cap.h,
			      n->stride > 0 ? n->stride : cap.w, cap.h, fmt);
	st = imresize_t(src, g_rga_dst, 0, 0, 0, 1);
	if (st != IM_STATUS_SUCCESS) {
		atomic_fetch_add(&g_rga_err, 1);
		if (!g_rga_msg[0])
			snprintf(g_rga_msg, sizeof(g_rga_msg), "%s",
				 imStrError(st));
	} else {
		atomic_fetch_add(&g_rga_ok, 1);
	}
	lat_add(&rga_lat, now_us() - t0);
}
#endif

/* ---------------- 阶段心跳(卡死后靠它定位死在哪一步) ---------------- */
static int g_stage_fd = -1;

static void stage(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	int n;

	if (g_stage_fd < 0)
		g_stage_fd = open("/root/camd_stage.log",
				  O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (g_stage_fd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		(void)!write(g_stage_fd, b, (size_t)(n < (int)sizeof(b) ? n : (int)sizeof(b) - 2));
	(void)!write(g_stage_fd, "\n", 1);
	fsync(g_stage_fd);
}

/* ---------------- RKNN 消费者(零拷贝:RGA 直写 NPU 输入 tensor) ---------------- */
static const char *g_rknn_model;	/* --rknn <model> 打开 */
#ifdef HAVE_RKNN
#define MAX_OUT 16
static rknn_context g_npu_ctx;
static rknn_tensor_mem *g_npu_in;
static rknn_tensor_attr g_npu_out_attr[MAX_OUT];
static unsigned g_npu_nout;
static int g_npu_w, g_npu_h;
static rga_buffer_t g_npu_dst;
static atomic_uint g_npu_ok, g_npu_err;
static char g_npu_msg[192];
static lat_t npu_lat, npu_rga_lat, npu_post_lat;
static unsigned g_det_total, g_det_max;
static atomic_uint g_score_maxi;	/* maxscore x1000 */
static char g_det_last[128];
static float g_obj_thresh;		/* 0 = 用 OBJ_THRESH */
static char *g_dump_rgb;		/* 调试:把 NPU 输入(640x640 RGB)存一份 */
static int g_dump_done;

/* YOLOv8 后处理(9 输出 = 3 尺度 x [DFL 64ch, 80 类, 1 score]),从 zerocopy/zc_npu.cpp 移植 */
#define OBJ_THRESH 0.35f
#define NMS_THRESH 0.45f
#define MAX_BOX 512

struct dbox {
	float x1, y1, x2, y2, score;
	int cls;
};

static int cmp_box(const void *a, const void *b)
{
	const struct dbox *x = a, *y = b;

	return (x->score < y->score) - (x->score > y->score);
}

static float box_iou(const struct dbox *a, const struct dbox *b)
{
	float xx1 = a->x1 > b->x1 ? a->x1 : b->x1;
	float yy1 = a->y1 > b->y1 ? a->y1 : b->y1;
	float xx2 = a->x2 < b->x2 ? a->x2 : b->x2;
	float yy2 = a->y2 < b->y2 ? a->y2 : b->y2;
	float w = xx2 - xx1 > 0 ? xx2 - xx1 : 0;
	float h = yy2 - yy1 > 0 ? yy2 - yy1 : 0;
	float inter = w * h;
	float ua = (a->x2 - a->x1) * (a->y2 - a->y1) +
		   (b->x2 - b->x1) * (b->y2 - b->y1) - inter;

	return ua > 0 ? inter / ua : 0;
}

static int decode_yolov8(rknn_tensor_attr *oa, int8_t **od, struct dbox *res,
			 int maxn, float *p_maxscore)
{
	static const int strides[3] = { 8, 16, 32 };
	int n = 0, s, y, x, k, side, c;

	*p_maxscore = 0;

	for (s = 0; s < 3; s++) {
		rknn_tensor_attr *areg = &oa[s * 3 + 0];
		rknn_tensor_attr *acls = &oa[s * 3 + 1];
		rknn_tensor_attr *ascr = &oa[s * 3 + 2];
		int gh = areg->dims[2], gw = areg->dims[3];
		int stride, idx;
		int8_t *reg, *cls, *scr;

		if (gh <= 0 || gw <= 0)
			continue;
		if ((int)acls->dims[2] != gh || (int)ascr->dims[2] != gh)
			continue;
		stride = strides[s];
		reg = od[s * 3 + 0];
		cls = od[s * 3 + 1];
		scr = od[s * 3 + 2];
		for (y = 0; y < gh; y++) {
			for (x = 0; x < gw; x++) {
				float score, bestv = 0, conf, d[4] = { 0, 0, 0, 0 };
				int best = -1;

				idx = y * gw + x;
				score = (scr[idx] - ascr->zp) * ascr->scale;
				if (score > *p_maxscore)
					*p_maxscore = score;
				if (score < (g_obj_thresh > 0 ? g_obj_thresh : OBJ_THRESH))
					continue;
				for (c = 0; c < 80; c++) {
					float v = (cls[(size_t)c * gh * gw + idx] - acls->zp) *
						  acls->scale;

					if (v > bestv) {
						bestv = v;
						best = c;
					}
				}
				if (best < 0)
					continue;
				conf = score * bestv;
				if (conf < (g_obj_thresh > 0 ? g_obj_thresh : OBJ_THRESH))
					continue;
				for (side = 0; side < 4; side++) {
					float p[16], mx = -1e9f, sum = 0, acc = 0;

					for (k = 0; k < 16; k++) {
						float v = (reg[((size_t)side * 16 + k) * gh * gw + idx] -
							   areg->zp) * areg->scale;

						p[k] = v;
						if (v > mx)
							mx = v;
					}
					for (k = 0; k < 16; k++) {
						p[k] = expf(p[k] - mx);
						sum += p[k];
					}
					for (k = 0; k < 16; k++)
						acc += (p[k] / sum) * k;
					d[side] = acc;
				}
				if (n < maxn) {
					float cx = (x + 0.5f) * stride, cy = (y + 0.5f) * stride;

					res[n].x1 = cx - d[0] * stride;
					res[n].y1 = cy - d[1] * stride;
					res[n].x2 = cx + d[2] * stride;
					res[n].y2 = cy + d[3] * stride;
					res[n].score = conf;
					res[n].cls = best;
					n++;
				}
			}
		}
	}
	/* 贪心 NMS */
	if (n > 1) {
		int keep = 0, i, j;

		qsort(res, (size_t)n, sizeof(res[0]), cmp_box);
		for (i = 0; i < n; i++) {
			int drop = 0;

			for (j = 0; j < keep; j++) {
				if (box_iou(&res[i], &res[j]) > NMS_THRESH) {
					drop = 1;
					break;
				}
			}
			if (!drop)
				res[keep++] = res[i];
		}
		n = keep;
	}
	return n;
}

static int rknn_consumer_init(void)
{
	rknn_input_output_num io;
	rknn_tensor_attr in_attr, in_native, oattr;
	FILE *f;
	long sz;
	void *model;
	int i, ret;

	f = fopen(g_rknn_model, "rb");
	if (!f) {
		snprintf(g_npu_msg, sizeof(g_npu_msg), "open %s: %s", g_rknn_model,
			 strerror(errno));
		return -1;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	model = malloc(sz);
	if (!model || fread(model, 1, sz, f) != (size_t)sz) {
		fclose(f);
		free(model);
		snprintf(g_npu_msg, sizeof(g_npu_msg), "read model failed");
		return -1;
	}
	fclose(f);
	ret = rknn_init(&g_npu_ctx, model, sz, 0, NULL);
	free(model);
	if (ret < 0) {
		snprintf(g_npu_msg, sizeof(g_npu_msg), "rknn_init=%d", ret);
		return -1;
	}
	rknn_query(g_npu_ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
	memset(&in_attr, 0, sizeof(in_attr));
	in_attr.index = 0;
	rknn_query(g_npu_ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
	/* 零拷贝必须用 native attr + pass_through=1,否则 runtime 每帧还会做一次转换 */
	memset(&in_native, 0, sizeof(in_native));
	in_native.index = 0;
	if (rknn_query(g_npu_ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &in_native,
		       sizeof(in_native)) != 0)
		in_native = in_attr;
	g_npu_h = in_native.dims[1];
	g_npu_w = in_native.dims[2];
	g_npu_in = rknn_create_mem(g_npu_ctx, in_native.size_with_stride);
	if (!g_npu_in) {
		snprintf(g_npu_msg, sizeof(g_npu_msg), "rknn_create_mem(input) failed");
		return -1;
	}
	in_native.pass_through = 1;
	ret = rknn_set_io_mem(g_npu_ctx, g_npu_in, &in_native);
	if (ret < 0) {
		snprintf(g_npu_msg, sizeof(g_npu_msg), "set_io_mem(input)=%d", ret);
		return -1;
	}
	g_npu_nout = io.n_output > MAX_OUT ? MAX_OUT : io.n_output;
	for (i = 0; i < (int)g_npu_nout; i++) {
		memset(&g_npu_out_attr[i], 0, sizeof(g_npu_out_attr[i]));
		g_npu_out_attr[i].index = i;
		/* 输出走 rknn_outputs_get(普通 NCHW 布局),不解码 native 的
		 * NC1HWC2 打包;输入仍然是 native 零拷贝,两者可以共存 */
		rknn_query(g_npu_ctx, RKNN_QUERY_OUTPUT_ATTR, &g_npu_out_attr[i],
			   sizeof(g_npu_out_attr[i]));
	}
	/* RGA 直接写 NPU 输入 buffer(同一个 DMA-BUF fd),全程零拷贝 */
	g_npu_dst = wrapbuffer_fd_t(g_npu_in->fd, g_npu_w, g_npu_h, g_npu_w * 3,
				    g_npu_h, RK_FORMAT_RGB_888);
	lat_init(&npu_lat, 4096);
	lat_init(&npu_rga_lat, 4096);
	printf(">>> RKNN 消费者: %s  输入 %dx%d type=%d zp=%d scale=%f 输出 %u 个\n",
	       g_rknn_model, g_npu_w, g_npu_h, in_native.type, in_native.zp,
	       in_native.scale, g_npu_nout);
	printf(">>> 零拷贝: RGA 写 fd=%d (mem %u B) -> rknn_run, pass_through=1\n",
	       g_npu_in->fd, g_npu_in->size);
	return 0;
}

static void rknn_process(struct node *n)
{
	int fmt = (cap.fourcc == V4L2_PIX_FMT_NV12) ? RK_FORMAT_YCbCr_420_SP
						    : RK_FORMAT_YCbCr_400;
	int64_t t0 = now_us(), t1, t2;
	rga_buffer_t src;
	IM_STATUS st;
	rknn_output outs[MAX_OUT];
	int8_t *odata[MAX_OUT];
	struct dbox boxes[MAX_BOX];
	float maxscore = 0;
	int ret, i, nd = 0;

	src = wrapbuffer_fd_t(n->dmabuf_fd, cap.w, cap.h,
			      n->stride > 0 ? n->stride : cap.w, cap.h, fmt);
	stage("rt seq=%u rga in fd=%d", n->seq, n->dmabuf_fd);
	st = imresize_t(src, g_npu_dst, 0, 0, 0, 1);
	stage("rt seq=%u rga out st=%d", n->seq, (int)st);
	if (st != IM_STATUS_SUCCESS) {
		atomic_fetch_add(&g_npu_err, 1);
		if (!g_npu_msg[0])
			snprintf(g_npu_msg, sizeof(g_npu_msg), "rga: %s",
				 imStrError(st));
		return;
	}
	if (g_dump_rgb && !g_dump_done && n->seq >= 10) {
		FILE *df = fopen(g_dump_rgb, "wb");

		if (df) {
			/* 在 xor 之前转储:这里才是 RGA 产出的 RGB888 */
			fwrite(g_npu_in->virt_addr, 1,
			       (size_t)g_npu_w * g_npu_h * 3, df);
			fclose(df);
			g_dump_done = 1;
			stage("rt seq=%u dump rgb %s", n->seq, g_dump_rgb);
		}
	}
	/* uint8 -> int8(zp=-128):减 128 在字节上等价于 xor 0x80,8 字节一次做完
	 * (与 zerocopy/zc_npu.cpp mode C 完全一致;RGA2 没有 NN 单元,只能这么补) */
	{
		uint64_t *t64 = (uint64_t *)g_npu_in->virt_addr;
		size_t n64 = (size_t)g_npu_w * g_npu_h * 3 / 8;
		size_t k;

		for (k = 0; k < n64; k++)
			t64[k] ^= 0x8080808080808080ULL;
	}
	t1 = now_us();
	lat_add(&npu_rga_lat, t1 - t0);
	stage("rt seq=%u npu run in", n->seq);
	ret = rknn_run(g_npu_ctx, NULL);
	stage("rt seq=%u npu run out ret=%d", n->seq, ret);
	if (ret < 0) {
		atomic_fetch_add(&g_npu_err, 1);
		if (!g_npu_msg[0])
			snprintf(g_npu_msg, sizeof(g_npu_msg), "rknn_run=%d", ret);
		return;
	}
	/* 输出用 rknn_outputs_get 取回(普通 NCHW int8),再做 YOLOv8 解码 + NMS */
	memset(outs, 0, sizeof(outs));
	for (i = 0; i < (int)g_npu_nout; i++) {
		outs[i].index = i;
		outs[i].want_float = 0;
		outs[i].is_prealloc = 0;
	}
	stage("rt seq=%u out get", n->seq);
	ret = rknn_outputs_get(g_npu_ctx, g_npu_nout, outs, NULL);
	if (ret < 0) {
		atomic_fetch_add(&g_npu_err, 1);
		if (!g_npu_msg[0])
			snprintf(g_npu_msg, sizeof(g_npu_msg), "outputs_get=%d", ret);
		return;
	}
	t2 = now_us();
	lat_add(&npu_lat, t2 - t1);
	for (i = 0; i < (int)g_npu_nout; i++)
		odata[i] = (int8_t *)outs[i].buf;
	nd = decode_yolov8(g_npu_out_attr, odata, boxes, MAX_BOX, &maxscore);
	rknn_outputs_release(g_npu_ctx, g_npu_nout, outs);
	lat_add(&npu_post_lat, now_us() - t2);
	if (maxscore * 1000 > (float)atomic_load(&g_score_maxi))
		atomic_store(&g_score_maxi, (unsigned)(maxscore * 1000));
	atomic_fetch_add(&g_npu_ok, 1);
	atomic_fetch_add(&g_det_total, (unsigned)nd);
	if ((unsigned)nd > atomic_load(&g_det_max))
		atomic_store(&g_det_max, (unsigned)nd);
	if (nd > 0 && !g_det_last[0])
		snprintf(g_det_last, sizeof(g_det_last), "cls=%d score=%.2f @%.0f,%.0f",
			 boxes[0].cls, boxes[0].score, boxes[0].x1, boxes[0].y1);
	if (nd > 0 && (n->seq % 30) == 0) {
		stage("rt seq=%u det=%d cls=%d score=%.2f max=%.2f box=%.0f,%.0f,%.0f,%.0f",
		      n->seq, nd, boxes[0].cls, boxes[0].score, maxscore,
		      boxes[0].x1, boxes[0].y1, boxes[0].x2, boxes[0].y2);
	} else if ((n->seq % 30) == 0) {
		stage("rt seq=%u det=0 max=%.2f", n->seq, maxscore);
	}
}
#endif

static int g_analyze_ms, g_ev_ms, g_frames_limit, g_recover_attempts;
static int g_fault_once;	/* 只注入一次故障:用于演示"恢复后能继续跑" */
static int g_rt_depth = 4, g_ev_depth = 2;
static int g_ev_policy;   /* 0=丢最旧(默认,不阻塞采集) 1=有界阻塞(背压) */
static int g_dq_timeout_ms, g_verbose, g_analyze_mode, g_fault;
static double g_ev_fps;
static const char *g_dev = "/dev/video0", *g_json;

static void fatal(int code, const char *fmt, ...);
static void node_release(struct node *n);

static void wake_all(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		pthread_mutex_lock(&q[i].m);
		pthread_cond_broadcast(&q[i].not_empty);
		pthread_cond_broadcast(&q[i].not_full);
		pthread_mutex_unlock(&q[i].m);
	}
}

static void fatal(int code, const char *fmt, ...)
{
	va_list ap;

	if (atomic_load(&g_fatal))
		return;
	va_start(ap, fmt);
	vsnprintf(g_fatal_msg, sizeof(g_fatal_msg), fmt, ap);
	va_end(ap);
	atomic_store(&g_fatal, code);
	atomic_store(&g_run, false);
	fprintf(stderr, "FATAL[%d]: %s\n", code, g_fatal_msg);
	wake_all();
}

/* 把 buffer 还给 V4L2 —— 只在所有引用都释放后发生 */
static void node_release(struct node *n)
{
	int prev = atomic_fetch_sub(&n->refs, 1);
	struct v4l2_buffer b;
	struct v4l2_plane pl;

	if (prev <= 0) {
		fprintf(stderr, "BUG: node seq=%llu refs 下溢\n",
			(unsigned long long)n->seq);
		return;
	}
	if (prev != 1)
		return;

	memset(&b, 0, sizeof(b));
	memset(&pl, 0, sizeof(pl));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = n->idx;
	b.length = 1;
	b.m.planes = &pl;
	if (ioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {
		atomic_fetch_add(&g_qbuf_err, 1);
		fprintf(stderr, "QBUF idx=%d 失败: %s\n", n->idx, strerror(errno));
	} else {
		atomic_fetch_add(&g_frames_done, 1);
	}
}

static void node_get(struct node *n)
{
	atomic_fetch_add(&n->refs, 1);
}

static void q_init(int id, int depth, int drop_oldest, int block_ms)
{
	queue_t *Q = &q[id];

	memset(Q, 0, sizeof(*Q));
	Q->cap = depth <= 0 ? 1 : (depth > MAX_Q ? MAX_Q : depth);
	Q->drop_oldest = drop_oldest;
	Q->block_ms = block_ms;
	pthread_mutex_init(&Q->m, NULL);
	pthread_cond_init(&Q->not_empty, NULL);
	pthread_cond_init(&Q->not_full, NULL);
}

/* 入队:接管调用方传入的这一份引用 */
static void q_push(int id, struct node *n)
{
	queue_t *Q = &q[id];
	struct node *victim = NULL;

	pthread_mutex_lock(&Q->m);
	while (Q->n == Q->cap && !Q->drop_oldest && atomic_load(&g_run)) {
		struct timespec dl;

		clock_gettime(CLOCK_REALTIME, &dl);
		dl.tv_sec += Q->block_ms / 1000;
		dl.tv_nsec += (long)(Q->block_ms % 1000) * 1000000L;
		if (dl.tv_nsec >= 1000000000L) {
			dl.tv_sec++;
			dl.tv_nsec -= 1000000000L;
		}
		if (pthread_cond_timedwait(&Q->not_full, &Q->m, &dl) == ETIMEDOUT)
			break;
	}
	if (Q->n == Q->cap) {
		if (Q->drop_oldest) {
			victim = Q->slot[Q->head];
			Q->head = (Q->head + 1) % Q->cap;
			Q->n--;
		} else {
			/* 背压超时:丢掉这一帧,但计数,保证不会无限堆 */
			Q->dropped++;
			pthread_mutex_unlock(&Q->m);
			node_release(n);
			return;
		}
	}
	Q->slot[(Q->head + Q->n) % Q->cap] = n;
	Q->n++;
	Q->pushed++;
	pthread_cond_signal(&Q->not_empty);
	pthread_mutex_unlock(&Q->m);

	if (victim) {
		Q->dropped++;
		node_release(victim);
	}
}

/* 出队;空且已 drain/停止时返回 NULL */
static struct node *q_pop(int id, int timeout_ms)
{
	queue_t *Q = &q[id];
	struct node *n = NULL;
	struct timespec dl;

	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += timeout_ms / 1000;
	dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) {
		dl.tv_sec++;
		dl.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&Q->m);
	while (Q->n == 0) {
		if ((atomic_load(&g_draining) || !atomic_load(&g_run)) &&
		    Q->n == 0)
			break;
		if (pthread_cond_timedwait(&Q->not_empty, &Q->m, &dl) == ETIMEDOUT)
			break;
	}
	if (Q->n > 0) {
		n = Q->slot[Q->head];
		Q->head = (Q->head + 1) % Q->cap;
		Q->n--;
		Q->popped++;
		pthread_cond_signal(&Q->not_full);
	}
	pthread_mutex_unlock(&Q->m);
	return n;
}

/* ---------------- 设备 ---------------- */
static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

/* rkcif 在 close() 之后释放 vb2 队列是延迟的(实测:紧接着重开会 S_FMT EBUSY,
 * 隔 100 ms 以上的新进程却能成功)。恢复路径必须容忍这个窗口,否则"有界恢复"
 * 会在第一次重开时就假失败。 */
static int xioctl_busy_retry(int fd, unsigned long req, void *arg, int ms)
{
	int waited = 0;

	while (1) {
		int r = ioctl(fd, req, arg);

		if (r >= 0 || errno != EBUSY || waited >= ms)
			return r;
		usleep(20000);
		waited += 20;
	}
}

static int dev_open(void)
{

	struct v4l2_format f;
	struct v4l2_requestbuffers req;
	int i;

	cap.fd = open(g_dev, O_RDWR | O_NONBLOCK);
	if (cap.fd < 0) {
		fatal(1, "open %s 失败: %s", g_dev, strerror(errno));
		return -1;
	}

	/* 这块 BSP 的采集节点是 multiplanar 接口(VIDIOC_*_MPLANE) */
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = cap.w;
	f.fmt.pix_mp.height = cap.h;
	f.fmt.pix_mp.pixelformat = cap.fourcc;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	if (xioctl_busy_retry(cap.fd, VIDIOC_S_FMT, &f, 1000) < 0) {
		fatal(1, "S_FMT %dx%d 失败: %s", cap.w, cap.h, strerror(errno));
		return -1;
	}
	if ((int)f.fmt.pix_mp.width != cap.w || (int)f.fmt.pix_mp.height != cap.h) {
		fprintf(stderr, "警告: 内核把分辨率改成 %ux%u\n",
			f.fmt.pix_mp.width, f.fmt.pix_mp.height);
		cap.w = f.fmt.pix_mp.width;
		cap.h = f.fmt.pix_mp.height;
	}

	memset(&req, 0, sizeof(req));
	req.count = cap.nbufs;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(cap.fd, VIDIOC_REQBUFS, &req) < 0) {
		fatal(1, "REQBUFS 失败: %s", strerror(errno));
		return -1;
	}
	if ((int)req.count < cap.nbufs)
		cap.nbufs = req.count;

	for (i = 0; i < cap.nbufs; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane pl;
		struct v4l2_exportbuffer exp;

		memset(&pl, 0, sizeof(pl));
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(cap.fd, VIDIOC_QUERYBUF, &b) < 0) {
			fatal(1, "QUERYBUF %d 失败: %s", i, strerror(errno));
			return -1;
		}

		memset(&exp, 0, sizeof(exp));
		exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		exp.index = i;
		exp.plane = 0;
		exp.flags = O_RDWR;
		if (xioctl(cap.fd, VIDIOC_EXPBUF, &exp) < 0) {
			fatal(1, "EXPBUF %d 失败: %s", i, strerror(errno));
			return -1;
		}

		cap.nodes[i] = calloc(1, sizeof(struct node));
		cap.nodes[i]->idx = i;
		cap.nodes[i]->len = pl.length;
		/* 本内核 v4l2_plane 没有 bytesperline 字段;用 S_FMT 回填的
		 * plane_fmt[0].bytesperline(1624x1224 BG10 实测 2048)。 */
		cap.nodes[i]->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline
					       ? (int)f.fmt.pix_mp.plane_fmt[0].bytesperline
					       : (int)cap.w;
		cap.nodes[i]->dmabuf_fd = exp.fd;
		cap.mmap[i] = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
				   MAP_SHARED, cap.fd, pl.m.mem_offset);
		if (cap.mmap[i] == MAP_FAILED)
			cap.mmap[i] = NULL;
		atomic_init(&cap.nodes[i]->refs, 0);
	}

#ifdef HAVE_RGA
	if ((g_rga || g_copy) && rga_init_dst() < 0) {
		fatal(1, "RGA 目标缓冲分配失败");
		return -1;
	}
#endif
	for (i = 0; i < cap.nbufs; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane pl;

		memset(&pl, 0, sizeof(pl));
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {
			fatal(1, "初始 QBUF %d 失败: %s", i, strerror(errno));
			return -1;
		}
	}
	return 0;
}

static int dev_streamon(void)
{
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (xioctl(cap.fd, VIDIOC_STREAMON, &t) < 0) {
		fatal(1, "STREAMON 失败: %s", strerror(errno));
		return -1;
	}
	atomic_store(&cap.streaming, true);
	return 0;
}

static void dev_streamoff(void)
{
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	int r;

	if (!atomic_load(&cap.streaming))
		return;
	r = xioctl(cap.fd, VIDIOC_STREAMOFF, &t);
	atomic_store(&cap.streaming, false);
	/* V4L2 规定 STREAMOFF 恒返回 0;真正的停流错误只能由驱动记录
	 * (ov8858_sm 的 last_stop_err 参数,见 docs/28 §2) */
	if (g_verbose)
		fprintf(stderr, "STREAMOFF ret=%d errno=%d\n", r, errno);
}

static void dev_close(void)
{
	int i;

	if (getenv("CAMDBG"))
		fprintf(stderr, "[dbg] dev_close: fd=%d nbufs=%d\n", cap.fd, cap.nbufs);

	/* 规范拆除顺序:先解除映射,再 REQBUFS(0) 让 vb2 真正释放 buffer,
	 * 最后才 close。只 close 而不 REQBUFS(0) 时,rkcif 侧的 vb2 队列会
	 * 继续处于 busy(num_buffers > 0),同一进程紧接着重开会 S_FMT EBUSY。 */
	for (i = 0; i < cap.nbufs; i++) {
		if (cap.mmap[i]) {
			munmap(cap.mmap[i], cap.nodes[i] ? cap.nodes[i]->len : 0);
			cap.mmap[i] = NULL;
		}
	}
	if (cap.fd > 0) {
		struct v4l2_requestbuffers req;

		memset(&req, 0, sizeof(req));
		req.count = 0;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		req.memory = V4L2_MEMORY_MMAP;
		if (ioctl(cap.fd, VIDIOC_REQBUFS, &req) < 0 && getenv("CAMDBG"))
			fprintf(stderr, "[dbg] REQBUFS(0) 失败: %s\n", strerror(errno));
	}
	for (i = 0; i < cap.nbufs; i++) {
		if (cap.nodes[i] && cap.nodes[i]->dmabuf_fd > 0) {
			int r = close(cap.nodes[i]->dmabuf_fd);

			if (getenv("CAMDBG"))
				fprintf(stderr, "[dbg] close dmabuf[%d]=%d rc=%d\n", i,
					cap.nodes[i]->dmabuf_fd, r);
			cap.nodes[i]->dmabuf_fd = -1;
		}
	}
	if (cap.fd > 0) {
		int r = close(cap.fd);

		if (getenv("CAMDBG"))
			fprintf(stderr, "[dbg] close dev fd=%d rc=%d\n", cap.fd, r);
		cap.fd = -1;
	}
	for (i = 0; i < cap.nbufs; i++) {
		if (cap.nodes[i]) {
			free(cap.nodes[i]);
			cap.nodes[i] = NULL;
		}
	}
}

/* ---------------- 采集线程 ---------------- */
static void *capture_thread(void *arg)
{
	(void)arg;
	while (atomic_load(&g_run)) {
		struct pollfd p = { .fd = cap.fd, .events = POLLIN };
		struct node *n;
		struct v4l2_buffer b;
		struct v4l2_plane pl;
		int pr = poll(&p, 1, g_dq_timeout_ms);

		if (pr == 0) {
			fprintf(stderr, "DQBUF 超时(%d ms)\n", g_dq_timeout_ms);
			continue;
		}
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fatal(1, "poll 失败: %s", strerror(errno));
			break;
		}
		memset(&pl, 0, sizeof(pl));
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				continue;
			fatal(1, "DQBUF 失败: %s", strerror(errno));
			break;
		}

		n = cap.nodes[b.index];
		n->seq = atomic_fetch_add(&g_frames, 1);
		n->ts_us = now_us();
		atomic_store(&n->refs, 1);	/* capture 自己一份 */
		if ((n->seq % 10) == 0)
			stage("cap seq=%u idx=%d", n->seq, b.index);

		pthread_mutex_lock(&g_first_mu);
		if (g_first_us == 0)
			g_first_us = n->ts_us;
		pthread_mutex_unlock(&g_first_mu);
		g_last_us = n->ts_us;

		/* 每个消费者各加一份引用,谁最后释放谁 QBUF */
		node_get(n);
		q_push(Q_RT, n);
		node_get(n);
		q_push(Q_EV, n);
		node_release(n);

		if (g_frames_limit > 0 &&
		    (int)atomic_load(&g_frames) >= g_frames_limit) {
			atomic_store(&g_draining, true);
			wake_all();
			break;
		}
	}
	return NULL;
}

/* ---------------- 实时分析消费者 ---------------- */
/* 消费者实现分派:同一处入口,便于给 CPU 拷贝 / RGA 两条路径统一计时 */
static int consumer_run(struct node *n)
{
#ifdef HAVE_RGA
	if (g_copy) {
		int64_t c0 = now_us();

		cpu_copy_process(n);
		lat_add(&rga_lat, now_us() - c0);
		atomic_fetch_add(&g_rga_ok, 1);
		return 0;
	}
	if (g_rga) {
		rga_process(n);
		return 0;
	}
#else
	if (g_copy || g_rga) {
		fprintf(stderr, "--rga/--cpu-copy 需要 -DHAVE_RGA 编译\\n");
		return -1;
	}
#endif
#ifdef HAVE_RKNN
	if (g_rknn_model) {
		rknn_process(n);
		return 0;
	}
#endif
	if (g_analyze_mode == 1 || g_fault == 1) {
		int ms = (g_fault == 1) ? g_analyze_ms * 10 : g_analyze_ms;

		if (ms < 1)
			ms = 1;
		if (ms > 20000)
			ms = 20000;
		usleep((useconds_t)ms * 1000);
	} else {
		/* nop:只触碰数据页,证明缓冲区可读(不做格式解释) */
		volatile unsigned char *p = (volatile unsigned char *)n;
		(void)p;
	}
	return 0;
}

static void *rt_thread(void *arg)
{
	(void)arg;
	while (1) {
		struct node *n = q_pop(Q_RT, 50);
		int64_t t0;

		if (!n) {
			if (atomic_load(&g_draining) || !atomic_load(&g_run))
				break;
			continue;
		}
		t0 = now_us();
		if (g_fault == 2) {	/* 模拟消费者内部故障 */
			if (g_fault_once)
				g_fault = 0;	/* 只坏这一帧:恢复后应能继续跑 */
			fatal(2, "实时分析消费者故障(注入)");
			node_release(n);
			break;
		}
		if (consumer_run(n) < 0) {
			node_release(n);
			break;
		}
		lat_add(&rt_lat, now_us() - t0);
		node_release(n);
	}
	return NULL;
}

/* ---------------- 取证/保存消费者(限速 + 背压) ---------------- */
static void *ev_thread(void *arg)
{
	int64_t next_us = 0;

	(void)arg;
	while (1) {
		struct node *n;
		int64_t t0;

		if (g_ev_fps > 0) {
			int64_t period = (int64_t)(1000000.0 / g_ev_fps);
			int64_t d;

			if (next_us) {
				d = next_us - now_us();
				if (d > 0)
					usleep((useconds_t)d);
			}
			next_us = now_us() + period;
		}
		n = q_pop(Q_EV, 50);
		if (!n) {
			if (atomic_load(&g_draining) || !atomic_load(&g_run))
				break;
			continue;
		}
		t0 = now_us();
		if (g_ev_ms > 0)
			usleep((useconds_t)g_ev_ms * 1000);
		lat_add(&ev_lat, now_us() - t0);
		node_release(n);
	}
	return NULL;
}

/* ---------------- 指标与诊断包 ---------------- */
static void dump_json(FILE *f, int rc)
{
	fprintf(f, "{\n");
	fprintf(f, "  \"rc\": %d,\n", rc);
	fprintf(f, "  \"device\": \"%s\",\n", g_dev);
	fprintf(f, "  \"size\": \"%dx%d\",\n", cap.w, cap.h);
	fprintf(f, "  \"frames\": %u,\n", atomic_load(&g_frames));
	fprintf(f, "  \"frames_returned\": %u,\n", atomic_load(&g_frames_done));
	fprintf(f, "  \"qbuf_errors\": %u,\n", atomic_load(&g_qbuf_err));
	fprintf(f, "  \"fatal\": %d,\n", atomic_load(&g_fatal));
	fprintf(f, "  \"fatal_msg\": \"%s\",\n", g_fatal_msg);
	fprintf(f, "  \"first_frame_ms\": %.2f,\n",
		g_first_us ? (g_first_us - g_start_us) / 1000.0 : -1.0);
	fprintf(f, "  \"elapsed_ms\": %.2f,\n",
		g_last_us ? (g_last_us - g_start_us) / 1000.0 : -1.0);
	fprintf(f, "  \"fps\": %.2f,\n",
		(g_last_us > g_start_us && g_first_us)
			? (double)atomic_load(&g_frames) * 1e6 /
				  (double)(g_last_us - g_first_us)
			: -1.0);
	fprintf(f,
		"  \"rt\": {\"processed\": %llu, \"dropped\": %llu, \"avg_us\": %.1f, \"max_us\": %llu, \"p95_us\": %.0f, \"p99_us\": %.0f},\n",
		(unsigned long long)rt_lat.n, (unsigned long long)q[Q_RT].dropped,
		rt_lat.n ? (double)rt_lat.sum_us / rt_lat.n : -1.0,
		(unsigned long long)rt_lat.max_us, lat_pct(&rt_lat, 0.95),
		lat_pct(&rt_lat, 0.99));
	fprintf(f,
		"  \"ev\": {\"processed\": %llu, \"dropped\": %llu, \"avg_us\": %.1f, \"max_us\": %llu, \"p95_us\": %.0f, \"p99_us\": %.0f},\n",
		(unsigned long long)ev_lat.n, (unsigned long long)q[Q_EV].dropped,
		ev_lat.n ? (double)ev_lat.sum_us / ev_lat.n : -1.0,
		(unsigned long long)ev_lat.max_us, lat_pct(&ev_lat, 0.95),
		lat_pct(&ev_lat, 0.99));
#ifdef HAVE_RGA
	if (g_rga || g_copy)
		fprintf(f,
			"  \"consumer\": {\"mode\": \"%s\", \"ok\": %u, \"err\": %u, \"dst\": \"%dx%d\", \"avg_us\": %.1f, \"max_us\": %llu, \"p99_us\": %.0f, \"msg\": \"%s\"},\n",
			g_copy ? "cpu-copy" : "rga",
			atomic_load(&g_rga_ok), atomic_load(&g_rga_err),
			g_rga_dw, g_rga_dh,
			rga_lat.n ? (double)rga_lat.sum_us / rga_lat.n : -1.0,
			(unsigned long long)rga_lat.max_us, lat_pct(&rga_lat, 0.99),
			g_rga_msg);
#endif
#ifdef HAVE_RKNN
	if (g_rknn_model)
		fprintf(f,
			"  \"npu\": {\"model\": \"%s\", \"in\": \"%dx%d\", \"ok\": %u, \"err\": %u, \"rga_us\": %.1f, \"run_us\": %.1f, \"run_p99_us\": %.0f, \"post_us\": %.1f, \"det_total\": %u, \"det_max\": %u, \"score_max\": %.3f, \"det_last\": \"%s\", \"msg\": \"%s\"},\n",
			g_rknn_model, g_npu_w, g_npu_h,
			atomic_load(&g_npu_ok), atomic_load(&g_npu_err),
			npu_rga_lat.n ? (double)npu_rga_lat.sum_us / npu_rga_lat.n : -1.0,
			npu_lat.n ? (double)npu_lat.sum_us / npu_lat.n : -1.0,
			lat_pct(&npu_lat, 0.99),
			npu_post_lat.n ? (double)npu_post_lat.sum_us / npu_post_lat.n : -1.0,
			atomic_load(&g_det_total), atomic_load(&g_det_max),
			atomic_load(&g_score_maxi) / 1000.0,
			g_det_last, g_npu_msg);
#endif
	fprintf(f, "  \"queue\": {\"rt_cap\": %d, \"ev_cap\": %d}\n", q[Q_RT].cap,
		q[Q_EV].cap);
	fprintf(f, "}\n");
}

static void diag_bundle(int rc)
{
	char dir[128], cmd[512];
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	snprintf(dir, sizeof(dir), "/root/camd_diag_%04d%02d%02d_%02d%02d%02d",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		 tm.tm_min, tm.tm_sec);
	mkdir(dir, 0777);

	snprintf(cmd, sizeof(cmd), "%s/metrics.json", dir);
	{
		FILE *f = fopen(cmd, "w");

		if (f) {
			dump_json(f, rc);
			fclose(f);
		}
	}
	snprintf(cmd, sizeof(cmd), "dmesg | tail -n 200 > %s/dmesg.txt", dir);
	if (system(cmd))
		;
	snprintf(cmd, sizeof(cmd), "cat /proc/meminfo > %s/meminfo.txt", dir);
	if (system(cmd))
		;
	snprintf(cmd, sizeof(cmd), "ls -l /proc/%d/fd > %s/fds.txt 2>&1",
		 getpid(), dir);
	if (system(cmd))
		;
	snprintf(cmd, sizeof(cmd),
		 "cat /sys/module/ov8858_sm/parameters/last_stop_err > %s/last_stop_err.txt 2>&1",
		 dir);
	if (system(cmd))
		;
	snprintf(cmd, sizeof(cmd),
		 "cat /proc/%d/status | grep -e VmRSS -e Threads > %s/threads.txt 2>&1",
		 getpid(), dir);
	if (system(cmd))
		;
	fprintf(stderr, "诊断包: %s\n", dir);
}

/* ---------------- 一次会话 ---------------- */
static int session(void)
{
	pthread_t t_cap, t_rt, t_ev;
	int rc = 0;

	atomic_store(&g_run, true);
	atomic_store(&g_draining, false);
	atomic_store(&g_fatal, 0);
	atomic_store(&g_frames, 0);
	atomic_store(&g_frames_done, 0);
	atomic_store(&g_qbuf_err, 0);
	atomic_store(&cap.streaming, false);
	g_first_us = g_last_us = 0;
	g_fatal_msg[0] = 0;
	lat_reset(&rt_lat);
	lat_reset(&ev_lat);

	if (g_rga)
		printf(">>> 实时消费者: RGA 零拷贝 -> %dx%d\n", g_rga_dw, g_rga_dh);
	if (g_copy)
		printf(">>> 实时消费者: CPU 拷贝+降采样 -> %dx%d\n", g_rga_dw, g_rga_dh);
	q_init(Q_RT, g_rt_depth, 1, 0);		/* 实时:丢最旧 */
	/* 取证队列:默认丢最旧(采集永不被消费者拖住);--ev-policy block 才做背压 */
	/* 配置自洽性检查:每个消费者在处理中会占住 1 个 buffer,采集侧还要 1 个在途,
	 * 队列里缓存的每个节点也都占着 buffer。buffer 数不够时不是"丢帧",而是采集被
	 * 饿死(DQBUF 拿不到空闲 buffer),表现为帧率断崖。 */
	{
		int need = g_rt_depth + g_ev_depth + 2;   /* +1 处理中 / 各 1 + 采集在途 1 */
		if (need > cap.nbufs)
			fprintf(stderr,
			        "WARN: 配置不自洽 —— buffer=%d, 但 rt_depth(%d)+ev_depth(%d)+2=%d\n"
			        "      ⥀ 采集可能被饿死(帧率断崖)。建议 -b %d 或降低队列深度。\n",
			        cap.nbufs, g_rt_depth, g_ev_depth, need, need);
	}

	q_init(Q_EV, g_ev_depth, g_ev_policy == 0 ? 1 : 0, 200);

#ifdef HAVE_RKNN
	if (g_rknn_model && rknn_consumer_init() < 0) {
		fprintf(stderr, "RKNN 消费者初始化失败: %s\n", g_npu_msg);
		return 1;
	}
#endif
	if (dev_open() < 0) {
		rc = 1;
		goto out;
	}
	stage("dev_open ok %dx%d bufs=%d fourcc=%08x", cap.w, cap.h, cap.nbufs,
	      cap.fourcc);
	if (dev_streamon() < 0) {
		rc = 1;
		goto out;
	}
	stage("streamon ok");

	g_start_us = now_us();
	pthread_create(&t_cap, NULL, capture_thread, NULL);
	pthread_create(&t_rt, NULL, rt_thread, NULL);
	pthread_create(&t_ev, NULL, ev_thread, NULL);

	pthread_join(t_cap, NULL);
	atomic_store(&g_draining, true);
	wake_all();
	pthread_join(t_rt, NULL);
	pthread_join(t_ev, NULL);

	dev_streamoff();
	rc = atomic_load(&g_fatal);
out:
	dev_close();
	if (rc)
		diag_bundle(rc);
	return rc;
}

/* ---------------- 入口 ---------------- */
static void usage(void)
{
	fprintf(stderr,
		"用法: camd [选项]\n"
		"  -d DEV            设备(默认 /dev/video0)\n"
		"  -w W -h H         分辨率(默认 1632x1224)\n"
		"  -f FOURCC         BG10/NV12(默认 BG10)\n"
		"  -n N              处理 N 帧后收尾(0=一直跑)\n"
		"  -b N              V4L2 buffer 数(默认 4)\n"
		"  --rt-depth N      实时队列深度(默认 4,满则丢最旧)\n"
		"  --ev-depth N      取证队列深度(默认 2)\n"
"  --ev-policy drop|block  取证队列满时策略(默认 drop=丢最旧,不阻塞采集)\n"
"  --rga             实时消费者改用 RGA 零拷贝缩放(需 -DHAVE_RGA -lrga)\n"
"  --rga-size WxH    RGA 目标尺寸(默认 640x360)\n"
"  --cpu-copy        对照组:CPU memcpy 整帧 + 降采样(不开 RGA)\n"
"  --rknn MODEL      实时消费者改用 RKNN 推理(RGA 直写 NPU 输入,零拷贝)\n"
"  --obj-thresh F    检测置信度阈值(默认 0.35,调试时可降到 0.10)\n"
"  --fault-once      故障只注入一次(验证恢复后能继续跑)\n"
		"  --ev-fps F        取证限速帧率(默认 5)\n"
		"  --analyze-ms N    实时分析模拟耗时(ms)\n"
		"  --analyze MODE    nop|sleep(默认 nop)\n"
		"  --ev-ms N         取证明细模拟耗时(ms)\n"
		"  --fault NAME      none|slow|fail\n"
		"  --recover-attempts N  有界恢复次数(默认 1)\n"
		"  --dq-timeout MS   DQBUF 超时(默认 2000)\n"
		"  --json PATH       指标输出文件\n"
		"  -v                详细日志\n");
}

int main(int argc, char **argv)
{
	const char *fcc = "BG10";
	int w = 1632, h = 1224, i, attempt = 0, rc;
	struct sigaction sa;
	static struct option lo[] = {
		{ "rt-depth", required_argument, 0, 1000 },
		{ "ev-depth", required_argument, 0, 1001 },
		{ "ev-fps", required_argument, 0, 1002 },
		{ "analyze-ms", required_argument, 0, 1003 },
		{ "analyze", required_argument, 0, 1004 },
		{ "ev-ms", required_argument, 0, 1005 },
		{ "fault", required_argument, 0, 1006 },
		{ "recover-attempts", required_argument, 0, 1007 },
		{ "dq-timeout", required_argument, 0, 1008 },
		{ "json", required_argument, 0, 1009 },
		{ "ev-policy", required_argument, 0, 1010 },
		{ "rga", no_argument, 0, 1011 },
		{ "rga-size", required_argument, 0, 1012 },
		{ "cpu-copy", no_argument, 0, 1013 },
		{ "fault-once", no_argument, 0, 1014 },
		{ "rknn", required_argument, 0, 1015 },
		{ "obj-thresh", required_argument, 0, 1016 },
		{ "dump-rgb", required_argument, 0, 1017 },
		{ 0, 0, 0, 0 }
	};

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	while ((i = getopt_long(argc, argv, "d:w:h:f:n:b:v", lo, NULL)) != -1) {
		switch (i) {
		case 'd': g_dev = optarg; break;
		case 'w': w = atoi(optarg); break;
		case 'h': h = atoi(optarg); break;
		case 'f': fcc = optarg; break;
		case 'n': g_frames_limit = atoi(optarg); break;
		case 'b': cap.nbufs = atoi(optarg); break;
		case 'v': g_verbose = 1; break;
		case 1000: g_rt_depth = atoi(optarg); break;
		case 1001: g_ev_depth = atoi(optarg); break;
		case 1002: g_ev_fps = atof(optarg); break;
		case 1003: g_analyze_ms = atoi(optarg); break;
		case 1004:
			g_analyze_mode = !strcmp(optarg, "sleep") ? 1 : 0;
			break;
		case 1005: g_ev_ms = atoi(optarg); break;
		case 1006:
			if (!strcmp(optarg, "slow"))
				g_fault = 1;
			else if (!strcmp(optarg, "fail"))
				g_fault = 2;
			break;
		case 1007: g_recover_attempts = atoi(optarg); break;
		case 1008: g_dq_timeout_ms = atoi(optarg); break;
		case 1009: g_json = optarg; break;
		case 1010:
			g_ev_policy = !strcmp(optarg, "block") ? 1 : 0;
			break;
		case 1011:
			g_rga = 1;
			break;
		case 1013:
			g_copy = 1;
			break;
		case 1014:
			g_fault_once = 1;
			break;
		case 1015:
			g_rknn_model = optarg;
			break;
		case 1016:
			g_obj_thresh = (float)atof(optarg);
			break;
		case 1017:
			g_dump_rgb = optarg;
			break;
		case 1012:
			sscanf(optarg, "%dx%d", &g_rga_dw, &g_rga_dh);
			break;
		default: usage(); return 2;
		}
	}

	if (cap.nbufs <= 0 || cap.nbufs > MAX_BUFS)
		cap.nbufs = 4;
	if (g_dq_timeout_ms <= 0)
		g_dq_timeout_ms = 2000;
	cap.w = w;
	cap.h = h;
	cap.fourcc = v4l2_fourcc(fcc[0], fcc[1], fcc[2], fcc[3]);
	lat_init(&rt_lat, 4096);
	lat_init(&ev_lat, 4096);

	for (i = 0; i < 2; i++) {
		pthread_mutex_init(&q[i].m, NULL);
		pthread_cond_init(&q[i].not_empty, NULL);
		pthread_cond_init(&q[i].not_full, NULL);
	}

	while (1) {
		rc = session();
		if (rc == 0 || attempt >= g_recover_attempts)
			break;
		attempt++;
		fprintf(stderr, ">>> 有界恢复 第 %d/%d 次\n", attempt,
			g_recover_attempts);
	}

	if (g_json) {
		FILE *f = fopen(g_json, "w");

		if (f) {
			dump_json(f, rc);
			fclose(f);
		} else {
			dump_json(stdout, rc);
		}
	} else {
		dump_json(stdout, rc);
	}
	return rc;
}
