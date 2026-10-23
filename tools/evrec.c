/*
 * evrec.c —— 事件前后录像（.h264 + .json）
 *
 * 采集 /dev/video5 → MPP 硬编 H.264 → 环形缓冲（按字节上限）
 * 收到 SIGUSR1（或触发文件变化）后，继续录 post 秒，然后把
 * "最近一个 IDR 起的 pre 段 + post 段" 写成 .h264，并写同名 .json。
 *
 * 板载编译:
 *   gcc -O2 -o evrec evrec.c -lrockchip_mpp -lpthread
 * 用法:
 *   ./evrec -d /dev/video5 -w 1920 -h 1080 --pre 3 --post 2 \
 *           --ring-mb 64 -o /root/ev/ev1 --trigger /tmp/evrec.trigger
 * 触发:
 *   kill -USR1 $(pidof evrec)     或   echo 1 > /tmp/evrec.trigger
 *
 * 设计约束（判据）:
 *   1) 环形缓冲只能从 IDR 起算，dump 时丢弃起点之前的 P 帧；
 *   2) pre 由时长决定，但受 --ring-mb 字节上限约束，冲突时以字节为准；
 *   3) .json 记录每帧 seq/ts/key，解码帧数必须与之相等才算切片正确。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <math.h>
#ifdef HAVE_RKNN
#include <rknn_api.h>
#endif
#ifdef HAVE_RGA
#include <rga/im2d.h>
#include <rga/rga.h>
#endif
#include <linux/videodev2.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>

#define MAX_FRAMES 4096

struct rec {
	unsigned char *data;
	int len;
	double ts_ms;		/* 自启动 */
	unsigned seq;
	int key;
	double abs_ms;		/* 绝对 CLOCK_REALTIME, 写 json 用 */
};

static struct rec ring[MAX_FRAMES];
static int ring_head, ring_cnt;
static unsigned g_pre_seq;      /* pre 窗口首帧的绝对 seq */
static unsigned long g_pushed;  /* 累计入环帧数(诊断) */
static int g_events, g_max_events;
static char g_outbuf[640];
static size_t ring_bytes, ring_limit = 64u << 20;
static double t0_ms;
static volatile sig_atomic_t g_trigger;
static const char *g_trig_path;

static double now_ms(clockid_t c)
{
	struct timespec ts;

	clock_gettime(c, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void on_sig(int s)
{
	(void)s;
	g_trigger = 1;
}

/* ---------------- V4L2 ---------------- */
static struct {
	int fd, nbufs;
	unsigned w, h;
	void *map[8];
	size_t len[8];
} cap;

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static int cap_open(const char *dev, int w, int h, int nbufs)
{
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	int i;

	cap.fd = open(dev, O_RDWR);
	if (cap.fd < 0) {
		perror("open");
		return -1;
	}
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (xioctl(cap.fd, VIDIOC_S_FMT, &f) < 0) {
		perror("S_FMT");
		return -1;
	}
	cap.w = f.fmt.pix_mp.width;
	cap.h = f.fmt.pix_mp.height;

	memset(&rb, 0, sizeof(rb));
	rb.count = nbufs;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (xioctl(cap.fd, VIDIOC_REQBUFS, &rb) < 0) {
		perror("REQBUFS");
		return -1;
	}
	cap.nbufs = rb.count;
	for (i = 0; i < rb.count; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane pl;
		struct v4l2_exportbuffer ex;

		memset(&pl, 0, sizeof(pl));
		memset(&b, 0, sizeof(b));
		b.type = rb.type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(cap.fd, VIDIOC_QUERYBUF, &b) < 0) {
			perror("QUERYBUF");
			return -1;
		}
		cap.len[i] = pl.length;
		cap.map[i] = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
				  MAP_SHARED, cap.fd, pl.m.mem_offset);
		if (cap.map[i] == MAP_FAILED) {
			perror("mmap");
			return -1;
		}
		if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {
			perror("QBUF");
			return -1;
		}
		(void)ex;
	}
	{
		enum v4l2_buf_type t = rb.type;

		if (xioctl(cap.fd, VIDIOC_STREAMON, &t) < 0) {
			perror("STREAMON");
			return -1;
		}
	}
	return 0;
}

/* ---------------- MPP ---------------- */
static MppCtx g_ctx;
static MppApi *g_mpi;
static MppBufferGroup g_grp;
static MppBuffer g_fbuf;
static size_t g_fsize;
static unsigned char *g_hdr;
static size_t g_hdr_len;

static int enc_init(int w, int h, int kbps, int gop)
{
	MppEncCfg cfg;

	if (mpp_create(&g_ctx, &g_mpi) != MPP_OK ||
	    mpp_init(g_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
		fprintf(stderr, "mpp init 失败\n");
		return -1;
	}
	mpp_enc_cfg_init(&cfg);
	mpp_enc_cfg_set_s32(cfg, "prep:width", w);
	mpp_enc_cfg_set_s32(cfg, "prep:height", h);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", w);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", h);
	mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_target", kbps * 1000);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", kbps * 1000 * 17 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", kbps * 1000 * 15 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", 30);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", 30);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:gop", gop);
	mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
	mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
	mpp_enc_cfg_set_s32(cfg, "h264:level", 51);
	if (g_mpi->control(g_ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
		fprintf(stderr, "SET_CFG 失败\n");
		return -1;
	}
	{
		MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
		MppPacket hdr = NULL;

		g_mpi->control(g_ctx, MPP_ENC_SET_HEADER_MODE, &hm);

		if (g_mpi->control(g_ctx, MPP_ENC_GET_HDR_SYNC, &hdr) == MPP_OK && hdr) {
			g_hdr_len = mpp_packet_get_length(hdr);
			g_hdr = malloc(g_hdr_len);
			memcpy(g_hdr, mpp_packet_get_pos(hdr), g_hdr_len);
			mpp_packet_deinit(&hdr);
			printf(">>> SPS/PPS 头 %zu 字节\n", g_hdr_len);
		}
	}
	mpp_buffer_group_get_internal(&g_grp, MPP_BUFFER_TYPE_ION);
	g_fsize = (size_t)w * h * 3 / 2;
	if (mpp_buffer_get(g_grp, &g_fbuf, g_fsize) != MPP_OK) {
		fprintf(stderr, "buffer_get 失败\n");
		return -1;
	}
	return 0;
}


/* ---------------- ① RGA 缩放(验证移植的最小一步) ---------------- */
static int g_detect, g_rga_w = 640, g_rga_h = 360;
static unsigned long g_rga_sum, g_rga_n;
static unsigned char *g_rgb;
static unsigned char *g_int8;

#ifdef HAVE_RGA
static rga_buffer_t g_rga_src, g_rga_dst;
static unsigned char *g_rga_buf;

static void rga_init(void)
{
	g_rga_buf = malloc((size_t)g_rga_w * g_rga_h * 3 / 2);
	if (!g_rga_buf) {
		fprintf(stderr, "RGA 缓冲分配失败\n");
		exit(1);
	}
	g_rga_dst = wrapbuffer_virtualaddr_t(g_rga_buf, g_rga_w, g_rga_h,
					     g_rga_w, g_rga_h,
					     RK_FORMAT_YCbCr_420_SP);
}

/* 用 mmap 虚拟地址做源, 不需要 EXPBUF(第①步先证明缩放可用) */
static void rga_scale(const void *src_va, unsigned sw, unsigned sh)
{
	IM_STATUS st;

	g_rga_src = wrapbuffer_virtualaddr_t((void *)src_va, sw, sh, sw, sh,
					     RK_FORMAT_YCbCr_420_SP);
	st = imresize_t(g_rga_src, g_rga_dst, 0, 0, 0, 1);
	if (st != IM_STATUS_SUCCESS) {
		fprintf(stderr, "RGA 失败: %s\n", imStrError(st));
		return;
	}
	{	/* 采样均值: 与 camd 同口径(Y 平面每 16 像素取 1) */
		unsigned long sum = 0, n = 0;
		unsigned char *y = g_rgb;

		for (int i = 0; i < g_rga_w * g_rga_h; i += 16) {
			sum += y[i];
			n++;
		}
		g_rga_sum += sum;
		g_rga_n += n;
	}
}
#endif

#define OBJ_THRESH 0.35f
#define NMS_THRESH 0.45f
#define MAX_BOX 512
static float g_obj_thresh = OBJ_THRESH;
#define MAX_OUT 16
static int g_no_obj_mul = 0;
static float g_det_score = 0.45f;   /* 触发所需分数 */
static int g_det_frames = 3;        /* 连续命中帧数 */
static int g_streak;
static int g_npu_w = 640, g_npu_h = 640;
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
				for (c = 0; c < 80; c++) {
					float v = (cls[(size_t)c * gh * gw + idx] - acls->zp) *
						  acls->scale;

					if (v > bestv) {
						bestv = v;
						best = c;
					}
				}
				/* 官方 rknn_model_zoo 的 yolov8 demo 直接拿 class 概率当 score;
				 * 之前一律 obj*cls,会把弱纹理目标系统性压到阈值以下。*/
				conf = g_no_obj_mul ? bestv : score * bestv;
				if (conf > *p_maxscore)
					*p_maxscore = conf;
				if (best < 0 || conf < g_obj_thresh)
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
					float bw, bh;

					res[n].x1 = cx - d[0] * stride;
					res[n].y1 = cy - d[1] * stride;
					res[n].x2 = cx + d[2] * stride;
					res[n].y2 = cy + d[3] * stride;
					/* 退化框过滤:铺满整幅的巨大框是典型误检(实测整帧被判成 train),
					 * 不滤掉会把真正的小目标挡在 NMS 之外。*/
					bw = res[n].x2 - res[n].x1;
					bh = res[n].y2 - res[n].y1;
					if (bw > 0.80f * g_npu_w && bh > 0.80f * g_npu_h)
						continue;
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

/* ---------------- ② RKNN 推理(只统计 det 数与最大分, 不画框) ---------------- */
static const char *g_model = "/root/npu/examples/yolov8/model/yolov8n_rk3568_i8.rknn";
static unsigned g_det_total, g_det_max;
static float g_score_max;

#ifdef HAVE_RKNN
static rknn_context g_nctx;
static rknn_tensor_mem *g_in_mem;
static rknn_tensor_attr g_in_attr, g_out_attr[MAX_OUT];
static unsigned g_nout;

static int npu_init(void)
{
	int ret, sz;
	FILE *f;
	void *model;
	struct stat st;

	if (stat(g_model, &st) < 0) { fprintf(stderr, "模型不存在: %s\n", g_model); return -1; }
	f = fopen(g_model, "rb"); if (!f) return -1;
	model = malloc((size_t)st.st_size);
	if (!model || fread(model, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { fclose(f); return -1; }
	fclose(f);

	ret = rknn_init(&g_nctx, model, (uint32_t)st.st_size, 0, NULL);
	free(model);
	if (ret != RKNN_SUCC) { fprintf(stderr, "rknn_init %d\n", ret); return -1; }

	ret = rknn_query(g_nctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &g_in_attr, sizeof(g_in_attr));
	if (ret != RKNN_SUCC) { fprintf(stderr, "query native input %d\n", ret); return -1; }
	/* NATIVE 输入是 NHWC [1,640,640,3] */
	g_npu_h = (int)g_in_attr.dims[1];
	g_npu_w = (int)g_in_attr.dims[2];

	g_in_mem = rknn_create_mem(g_nctx, g_in_attr.size_with_stride);
	if (!g_in_mem) { fprintf(stderr, "create_mem 失败\n"); return -1; }
	g_in_attr.pass_through = 1;
	ret = RKNN_SUCC; /* 不绑 io_mem: 否则 rknn_inputs_set 的缓冲被忽略 */


	sz = g_npu_w * g_npu_h * 3;
	g_rgb  = malloc((size_t)sz);
	g_int8 = malloc((size_t)sz);
	if (!g_rgb || !g_int8) return -1;

	{	rknn_input_output_num io;
		unsigned k;

		memset(&io, 0, sizeof(io));
		ret = rknn_query(g_nctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
		if (ret != RKNN_SUCC) { fprintf(stderr, "query out num %d\n", ret); return -1; }
		g_nout = io.n_output > MAX_OUT ? MAX_OUT : io.n_output;
		for (k = 0; k < g_nout; k++) {
			memset(&g_out_attr[k], 0, sizeof(g_out_attr[k]));
			g_out_attr[k].index = k;   /* 必须显式设 index, 否则每次拿到的都是 0 号张量 */
			ret = rknn_query(g_nctx, RKNN_QUERY_OUTPUT_ATTR, &g_out_attr[k],
					 sizeof(g_out_attr[k]));
			if (ret != RKNN_SUCC) { fprintf(stderr, "query out attr %u\n", k); return -1; }
		}
	}
	if (g_det_score < g_obj_thresh)
		g_obj_thresh = g_det_score;
	printf(">>> NPU 就绪 %dx%d nout=%u\n", g_npu_w, g_npu_h, g_nout);
	return 0;
}

static void npu_run(const unsigned char *rgb)
{
	rknn_input in;
	rknn_output outs[MAX_OUT];
	struct dbox boxes[MAX_BOX];
	int ret, nd;
	float maxscore = 0.0f;
	size_t i, sz = (size_t)g_npu_w * g_npu_h * 3;

	for (i = 0; i < sz; i += 8) {      /* uint8 -> int8 零点偏置(zp=-128) */
		g_int8[i + 0] = rgb[i + 0] ^ 0x80;
		g_int8[i + 1] = rgb[i + 1] ^ 0x80;
		g_int8[i + 2] = rgb[i + 2] ^ 0x80;
		g_int8[i + 3] = rgb[i + 3] ^ 0x80;
		g_int8[i + 4] = rgb[i + 4] ^ 0x80;
		g_int8[i + 5] = rgb[i + 5] ^ 0x80;
		g_int8[i + 6] = rgb[i + 6] ^ 0x80;
		g_int8[i + 7] = rgb[i + 7] ^ 0x80;
	}

	memset(&in, 0, sizeof(in));
	in.index = 0;
	in.buf = g_int8;
	in.size = (uint32_t)sz;
	in.type = RKNN_TENSOR_INT8;
	in.fmt = RKNN_TENSOR_NHWC;
	in.pass_through = 1;
	{	/* 诊断: g_int8 是否真的有数据 */
		static int dbg;
		long s = 0;
		for (i = 0; i < sz; i += 64) s += (unsigned char)g_int8[i];
		if (dbg++ < 3)
			fprintf(stderr, "[npu] in: sz=%u mean=%ld rgb0=%u/%u/%u int8_0=%d\n",
				(uint32_t)sz, s / (long)(sz / 64),
				rgb[0], rgb[1], rgb[2], (int)(signed char)g_int8[0]);
	}

	ret = rknn_inputs_set(g_nctx, 1, &in);
	if (ret != RKNN_SUCC) {
		fprintf(stderr, "[npu] inputs_set ret=%d\n", ret);
		return;
	}
	ret = rknn_run(g_nctx, NULL);
	if (ret != RKNN_SUCC) {
		fprintf(stderr, "[npu] run ret=%d\n", ret);
		return;
	}

	memset(outs, 0, sizeof(outs));
	ret = rknn_outputs_get(g_nctx, g_nout, outs, NULL);
	if (ret != RKNN_SUCC) {
		fprintf(stderr, "[npu] outputs_get ret=%d nout=%u\n", ret, g_nout);
		return;
	}
	{	static int dbg2;
		if (dbg2++ < 2 && outs[0].buf)
			fprintf(stderr, "[npu] out0 size=%d b0..b7=%d %d %d %d %d %d %d %d\n",
				(int)outs[0].size,
				((int8_t *)outs[0].buf)[0], ((int8_t *)outs[0].buf)[1],
				((int8_t *)outs[0].buf)[2], ((int8_t *)outs[0].buf)[3],
				((int8_t *)outs[0].buf)[4], ((int8_t *)outs[0].buf)[5],
				((int8_t *)outs[0].buf)[6], ((int8_t *)outs[0].buf)[7]);
	}

	{	int8_t *od[MAX_OUT];
		for (i = 0; i < g_nout; i++)
			od[i] = (int8_t *)outs[i].buf;
		nd = decode_yolov8(g_out_attr, od, boxes, MAX_BOX, &maxscore);
	}
	rknn_outputs_release(g_nctx, g_nout, outs);

	{	/* 触发判定: 目标类 + 分数门限, 且必须连续 N 帧 */
		int hit = 0, k;

		for (k = 0; k < nd; k++)
			if (boxes[k].cls == 0 && boxes[k].score >= g_det_score) {
				hit = 1;
				break;
			}
		if (hit) {
			if (++g_streak >= g_det_frames && !g_trigger) {
				g_trigger = 1;
				printf(">>> 检测触发 streak=%d score=%.3f\n",
				       g_streak, (double)boxes[k].score);
			}
		} else {
			g_streak = 0;
		}
	}

	g_det_total += (unsigned)nd;
	if ((unsigned)nd > g_det_max) g_det_max = (unsigned)nd;
	if (maxscore > g_score_max) g_score_max = maxscore;
}
#endif

static void rga_to_rgb(void)
{
	g_rga_dst = wrapbuffer_virtualaddr_t(g_rgb, g_npu_w, g_npu_h,
					     g_npu_w, g_npu_h, RK_FORMAT_RGB_888);
}

/* ---------------- 环形缓冲 ---------------- */
static int is_key(const unsigned char *p, int len)
{
	int i;

	for (i = 0; i + 4 < len; i++) {
		if (p[i] != 0 || p[i + 1] != 0)
			continue;
		if (p[i + 2] == 1) {
			int t = p[i + 3] & 0x1f;

			if (t == 5 || t == 7)
				return 1;
			i += 2;
		}
	}
	return 0;
}

static void ring_push(unsigned char *d, int len, unsigned seq, int key)
{
	struct rec *r;

	if (ring_cnt == MAX_FRAMES || ring_bytes + (size_t)len > ring_limit) {
		r = &ring[ring_head];
		ring_bytes -= (size_t)r->len;
		free(r->data);
		ring_head = (ring_head + 1) % MAX_FRAMES;
		ring_cnt--;
	}
	r = &ring[(ring_head + ring_cnt) % MAX_FRAMES];
	r->data = d;
	r->len = len;
	r->seq = seq;
	r->key = key;
	r->ts_ms = now_ms(CLOCK_MONOTONIC) - t0_ms;
	r->abs_ms = now_ms(CLOCK_REALTIME);
	ring_cnt++;
	g_pushed++;
	ring_bytes += (size_t)len;
}

static struct rec *ring_at(int i)
{
	return &ring[(ring_head + i) % MAX_FRAMES];
}

/* 找到 >= from 的第一个 IDR 的下标 */
static int ring_first_idr_from(int from)
{
	int i;

	for (i = from; i < ring_cnt; i++)
		if (ring_at(i)->key)
			return i;
	return -1;
}

static int dump(const char *out, unsigned trig_seq, double trig_ts, int pre_i,
		int end_i, unsigned w, unsigned h, double fps)
{
	char path[512], jpath[512];
	FILE *f, *j;
	int i, n = 0;

	snprintf(path, sizeof(path), "%s.h264", out);
	snprintf(jpath, sizeof(jpath), "%s.json", out);
	f = fopen(path, "wb");
	if (!f) {
		perror("fopen h264");
		return -1;
	}
	for (i = pre_i; i <= end_i && i < ring_cnt; i++) {
		struct rec *r = ring_at(i);

		if (0 && r->key && g_hdr_len)
			fwrite(g_hdr, 1, g_hdr_len, f);
		fwrite(r->data, 1, (size_t)r->len, f);
		n++;
	}
	fclose(f);

	j = fopen(jpath, "w");
	if (!j)
		return -1;
	fprintf(j, "{\"file\":\"%s.h264\",\"trigger_seq\":%u,"
		"\"trigger_ts_ms\":%.1f,\"first_idr_seq\":%u,"
		"\"first_idr_ts_ms\":%.1f,\"frames\":%d,"
		"\"width\":%u,\"height\":%u,\"fps\":%.2f,\"list\":[",
		out, trig_seq, trig_ts, ring_at(pre_i)->seq,
		ring_at(pre_i)->ts_ms, n, w, h, fps);
	for (i = pre_i; i <= end_i && i < ring_cnt; i++) {
		struct rec *r = ring_at(i);

		fprintf(j, "%s{\"seq\":%u,\"ts_ms\":%.1f,\"abs_ms\":%.1f,"
			"\"bytes\":%d,\"key\":%d}",
			(i == pre_i) ? "" : ",", r->seq, r->ts_ms,
			r->abs_ms, r->len, r->key);
	}
	fprintf(j, "]}\n");
	fclose(j);
	printf(">>> NPU det_total=%u det_max=%u score_max=%.3f\n",
	       g_det_total, g_det_max, (double)g_score_max);
	if (g_rga_n)
		printf(">>> RGA 缩放均值 Y=%.1f (%ux%u, 采样 %lu)\n",
		       (double)g_rga_sum / g_rga_n, g_rga_w, g_rga_h, g_rga_n);
	printf(">>> 写出 %s.h264 (%d 帧) + %s.json\n", out, n, out);
	return n;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video5", *out = "/root/ev/ev";
	int w = 1920, h = 1080, pre_sec = 3, post_sec = 2, kbps = 8000, gop;
	int i, k, trigger_i = -1, pre_i = -1, end_i = -1;
	unsigned seq = 0;
	double trig_ts = 0, trig_abs = 0, t_start;
	struct stat st;
	time_t trig_mtime = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d")) dev = argv[++i];
		else if (!strcmp(argv[i], "-w")) w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h")) h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pre")) pre_sec = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--post")) post_sec = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ring-mb")) ring_limit = (size_t)atoi(argv[++i]) << 20;
		else if (!strcmp(argv[i], "-o")) out = argv[++i];
		else if (!strcmp(argv[i], "--trigger")) g_trig_path = argv[++i];
		else if (!strcmp(argv[i], "--kbps")) kbps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--detect")) g_detect = 1;
		else if (!strcmp(argv[i], "--model")) g_model = argv[++i];
		else if (!strcmp(argv[i], "--max-events")) g_max_events = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--det-score")) g_det_score = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--det-frames")) g_det_frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--obj-thresh")) g_obj_thresh = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--rga-size")) sscanf(argv[++i], "%dx%d", &g_rga_w, &g_rga_h);
	}
	gop = (pre_sec + post_sec) * 15 / 2;
	if (gop < 8)
		gop = 8;

	signal(SIGUSR1, on_sig);
	t0_ms = now_ms(CLOCK_MONOTONIC);
	t_start = t0_ms;

	if (cap_open(dev, w, h, 4) < 0)
		return 1;
	if (enc_init(w, h, kbps, gop) < 0)
		return 1;
#ifdef HAVE_RGA
	if (g_detect) {
		rga_init();
#ifdef HAVE_RKNN
		if (npu_init() < 0)
			return 1;
		rga_to_rgb();
#endif
	}
#endif
	{	/* 输出名加时间戳: 长跑/多次启动不会覆盖同名文件 */
		time_t now_t = time(NULL);
		struct tm tmv;
		char tstr[32];

		localtime_r(&now_t, &tmv);
		strftime(tstr, sizeof(tstr), "%Y%m%d-%H%M%S", &tmv);
		snprintf(g_outbuf, sizeof(g_outbuf), "%s_%s", out, tstr);
		out = g_outbuf;
	}
	printf(">>> 采集 %s %dx%d,pre=%ds post=%ds ring=%zuMB gop=%d\n",
	       dev, w, h, pre_sec, post_sec, ring_limit >> 20, gop);

	for (;;) {
		struct v4l2_buffer b;
		struct v4l2_plane pl;
		MppFrame frame = NULL;
		MppPacket pkt = NULL;
		double ts;

		memset(&pl, 0, sizeof(pl));
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.length = 1;
		b.m.planes = &pl;
		if (xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EINTR)
				continue;
			perror("DQBUF");
			break;
		}
		ts = now_ms(CLOCK_MONOTONIC);
#ifdef HAVE_RGA
#ifdef HAVE_RKNN
		if (g_detect) {
			rga_scale(cap.map[b.index], w, h);
			npu_run(g_rgb);
		}
#else
		if (g_detect)
			rga_scale(cap.map[b.index], w, h);
#endif
#endif
		memcpy(mpp_buffer_get_ptr(g_fbuf), cap.map[b.index],
		       (size_t)w * h * 3 / 2);
		xioctl(cap.fd, VIDIOC_QBUF, &b);

		mpp_frame_init(&frame);
		mpp_frame_set_width(frame, w);
		mpp_frame_set_height(frame, h);
		mpp_frame_set_hor_stride(frame, w);
		mpp_frame_set_ver_stride(frame, h);
		mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
		mpp_frame_set_buffer(frame, g_fbuf);
		if (g_mpi->encode_put_frame(g_ctx, frame) != MPP_OK) {
			mpp_frame_deinit(&frame);
			continue;
		}
		mpp_frame_deinit(&frame);

		while (g_mpi->encode_get_packet(g_ctx, &pkt) == MPP_OK && pkt) {
			void *pos = mpp_packet_get_pos(pkt);
			size_t len = mpp_packet_get_length(pkt);
			unsigned char *copy;

			if (len == 0) {
				mpp_packet_deinit(&pkt);
				continue;
			}
			copy = malloc(len);
			memcpy(copy, pos, len);
			ring_push(copy, (int)len, seq, is_key(copy, (int)len));
			seq++;
			mpp_packet_deinit(&pkt);
		}

		/* 触发: 信号 或 触发文件 mtime 变化 */
		if (!g_trigger && g_trig_path && stat(g_trig_path, &st) == 0 &&
		    st.st_mtime != trig_mtime) {
			trig_mtime = st.st_mtime;
			g_trigger = 1;
		}
		if (g_trigger && trigger_i < 0 && ring_cnt < (int)(pre_sec * 15)) {
			/* 预热未满: 环里的帧数还不够 pre 秒前置内容, 忽略本次触发,
			 * 但不清 g_streak, 攒够后会立刻重新触发 */
			g_trigger = 0;
		}
		if (g_trigger && trigger_i < 0) {
			trigger_i = ring_cnt;
			trig_ts = ts - t_start;
			trig_abs = now_ms(CLOCK_REALTIME);
			/* pre: 从触发点往前按时间回退 pre_sec */
			for (i = ring_cnt - 1; i >= 0; i--)
				if (ring_at(i)->ts_ms < trig_ts - pre_sec * 1000.0)
					break;
			/* 从回退点往前找最近的 IDR(往前才能把 pre 窗口填满;
			 * 往后找会跳到下一个 GOP 的 IDR, 白白丢掉中间一段) */
			pre_i = -1;
			for (i = ring_cnt - 1; i >= 0; i--)
				if (ring_at(i)->ts_ms < trig_ts - pre_sec * 1000.0)
					break;
			for (; i >= 0; i--)
				if (ring_at(i)->key) { pre_i = i; break; }
			if (pre_i < 0)
				pre_i = ring_first_idr_from(0);
			g_pre_seq = ring_at(pre_i)->seq;
			printf(">>> 触发 seq=%u ts=%.1fms -> pre_i=%d pre_seq=%u "
			       "(ring_cnt=%d head=%d pushed=%lu)\n",
			       seq, trig_ts, pre_i, g_pre_seq,
			       ring_cnt, ring_head, g_pushed);
		}
		if (trigger_i >= 0 && ts - t_start > trig_ts + post_sec * 1000.0) {
			char outn[512];
			int pi = pre_i;

			end_i = ring_cnt - 1;
			/* 触发到此刻环形可能已前移 -> 按 seq 重新定位 */
			if (g_pre_seq)
				for (k = 0; k < ring_cnt; k++)
					if (ring_at(k)->seq == g_pre_seq) { pi = k; break; }
			if (pi >= 0) {
				snprintf(outn, sizeof(outn), "%s-%02d", out, g_events);
				dump(outn, seq - 1, trig_ts, pi, end_i, w, h,
				     (double)(end_i - pi + 1) * 1000.0 /
				     (trig_ts + post_sec * 1000.0));
			}
			g_events++;
			printf(">>> 事件 %d 完成, 继续等待下一个事件\n", g_events);
			/* 复位状态, 复用同一块环 */
			ring_head = ring_cnt = 0;
			ring_bytes = 0;
			g_pushed = 0;
			g_trigger = 0;
			g_streak = 0;
			g_pre_seq = 0;
			trigger_i = pre_i = end_i = -1;
			if (g_max_events && g_events >= g_max_events)
				break;
		}
	}

	{
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		xioctl(cap.fd, VIDIOC_STREAMOFF, &t);
	}
	/* 触发到 dump 之间环形可能已前移, 下标会失效 -> 用绝对 seq 重新定位 */
	if (end_i >= 0 && g_pre_seq) {
		for (i = 0; i < ring_cnt; i++)
			if (ring_at(i)->seq == g_pre_seq) {
				pre_i = i;
				break;
			}
	}
	if (pre_i < 0 || end_i < 0) {
		fprintf(stderr, "未触发或无数据, 不落盘\n");
		return 1;
	}
	dump(out, seq - 1, trig_ts, pre_i, end_i, w, h,
	     (double)(end_i - pre_i + 1) * 1000.0 / (trig_ts + post_sec * 1000.0));
	return 0;
}
