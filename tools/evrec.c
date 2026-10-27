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
 *   3) .json 逐包记录 seq(源帧序号)/pkt/ts/key, 1 包 = 1 解码帧时
 *      "解码帧数 == 记录数" 才算切片正确。
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
#include <stdint.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
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
	/* 源帧序号: 只有 DQBUF 成功才 +1。一帧可能吐出多个编码包,
	 * 这些包共用同一个 seq, 用 pkt 区分帧内第几个包。 */
	unsigned seq;
	unsigned pkt;		/* 帧内包序号, 0 起; 与 seq 一起唯一标识一个包 */
	int key;
	double abs_ms;		/* 绝对 CLOCK_REALTIME, 写 json 用 */
	double cap_ms;		/* vb2 采集时间戳(自启动); MONOTONIC flag 时与 ts_ms 同零点 */
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
static volatile sig_atomic_t g_stop;	/* SIGTERM: 优雅退出 */
static const char *g_trig_path;
static int g_osd;		/* --osd 烧 OSD; 默认关闭, 不加参数行为不变 */
static int g_recover = 2;	/* --recover N: 链路故障重开设备次数(0=不恢复) */
/* --fault-sim dqbuf:N[,N...]: 第 N 次 DQBUF 强制失败(仅测试) */
static int g_fault_dqbuf[8];
static int g_fault_done[8];
static int g_fault_cnt;
static unsigned g_dqbuf_calls;

/* 恢复后连续采到多少帧才算"链路稳定", 稳定后恢复计数复位 */
#define RECOVER_STABLE_FRAMES 30

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

/* 与 on_sig 分开: 退出信号绝不能顺带触发一次录像 */
static void on_term(int s)
{
	(void)s;
	g_stop = 1;
}

/* ---------------- V4L2 ---------------- */
static struct {
	int fd, nbufs;
	unsigned w, h, stride;	/* stride 来自 S_FMT 回填, 不等于 width */
	void *map[8];
	size_t len[8];
} cap = { .fd = -1 };

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
		/* g_stop 只由 SIGTERM 置位; 置位后不再重试 EINTR,
		 * 让阻塞中的 DQBUF 真正返回, 主线程才能 join 发送线程并退出 */
	} while (r < 0 && errno == EINTR && !g_stop);
	return r;
}

/*
 * 只释放采集侧: STREAMOFF -> munmap -> REQBUFS(count=0) -> close.
 * MPP 编码器与 RTSP 服务端一律不动 —— 恢复链路时它们必须活下来,
 * 否则 RTSP 客户端会断开、编码参数集也要重建。
 */
static void cap_release(void)
{
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers rb;
	int i;

	if (cap.fd < 0)
		return;
	xioctl(cap.fd, VIDIOC_STREAMOFF, &t);
	for (i = 0; i < cap.nbufs; i++) {
		if (cap.map[i] && cap.map[i] != MAP_FAILED)
			munmap(cap.map[i], cap.len[i]);
		cap.map[i] = NULL;
		cap.len[i] = 0;
	}
	memset(&rb, 0, sizeof(rb));
	rb.count = 0;		/* 释放 vb2 buffer */
	rb.type = t;
	rb.memory = V4L2_MEMORY_MMAP;
	xioctl(cap.fd, VIDIOC_REQBUFS, &rb);
	close(cap.fd);
	cap.fd = -1;
	cap.nbufs = 0;
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
		goto fail;
	}
	cap.w = f.fmt.pix_mp.width;
	cap.h = f.fmt.pix_mp.height;
	/* 1920x1080 实测 stride 可能不等于 1920: 必须用回填值算 Y 平面步长 */
	cap.stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (cap.stride < cap.w)
		cap.stride = cap.w;
	if (g_osd)
		printf(">>> S_FMT 回填 %ux%u stride=%u sizeimage=%u\n", cap.w, cap.h,
		       cap.stride, f.fmt.pix_mp.plane_fmt[0].sizeimage);

	memset(&rb, 0, sizeof(rb));
	rb.count = nbufs;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (xioctl(cap.fd, VIDIOC_REQBUFS, &rb) < 0) {
		perror("REQBUFS");
		goto fail;
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
			goto fail;
		}
		cap.len[i] = pl.length;
		cap.map[i] = mmap(NULL, pl.length, PROT_READ | PROT_WRITE,
				  MAP_SHARED, cap.fd, pl.m.mem_offset);
		if (cap.map[i] == MAP_FAILED) {
			perror("mmap");
			goto fail;
		}
		if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {
			perror("QBUF");
			goto fail;
		}
		(void)ex;
	}
	{
		enum v4l2_buf_type t = rb.type;

		if (xioctl(cap.fd, VIDIOC_STREAMON, &t) < 0) {
			perror("STREAMON");
			goto fail;
		}
	}
	return 0;
fail:
	/* 重开失败必须把半开的设备关掉, 否则恢复重试会 EBUSY/fd 泄漏 */
	cap_release();
	return -1;
}

/* ---------------- OSD: 源帧 seq + 相对时间(烧在 NV12 的 Y 平面) ---------------- */
/* 5x7 点阵, 每行 5 bit; 与 camd.c 同一份表(0-9 : . # t) */
static const unsigned char font5x7[14][7] = {
	{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},	/* 0 */
	{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},	/* 1 */
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},	/* 2 */
	{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},	/* 3 */
	{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},	/* 4 */
	{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},	/* 5 */
	{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},	/* 6 */
	{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},	/* 7 */
	{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},	/* 8 */
	{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},	/* 9 */
	{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},	/* : */
	{0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},	/* . */
	{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},	/* # */
	{0x04,0x04,0x0E,0x04,0x04,0x05,0x02},	/* t */
};

static unsigned g_osd_seq;	/* 源帧序号, 与写进 .json 的 seq/frame_seq 同源 */
static double g_osd_ms;		/* 相对首帧的毫秒数, 与 .json 的 ts_ms 同口径 */
static int g_ts_flags = -1;	/* 首帧的 V4L2_BUF_FLAG_TIMESTAMP_*(<0=还没见过帧) */

/* NV12 的 Y 平面单字节写: 步长用 S_FMT 回填的 stride, 不是 width */
static void put_y_px(unsigned char *y_plane, int stride, int W, int H,
		     int x, int y, unsigned char v)
{
	if (x < 0 || y < 0 || x >= W || y >= H)
		return;
	y_plane[(size_t)y * stride + x] = v;
}

static int glyph_idx(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch == ':')
		return 10;
	if (ch == '.')
		return 11;
	if (ch == '#')
		return 12;
	if (ch == 't')
		return 13;
	return -1;
}

static void draw_text(unsigned char *y_plane, int stride, int W, int H,
		      int x, int y0, const char *s, int sc)
{
	for (; *s; s++) {
		int gi = glyph_idx(*s), r, cc;

		if (gi < 0) {		/* 空格: 只前进 */
			x += 4 * sc;
			continue;
		}
		for (r = 0; r < 7; r++) {
			for (cc = 0; cc < 5; cc++) {
				int on = (font5x7[gi][r] >> (4 - cc)) & 1;
				int dy, dx;

				for (dy = 0; dy < sc; dy++)
					for (dx = 0; dx < sc; dx++)
						put_y_px(y_plane, stride, W, H,
							 x + cc * sc + dx,
							 y0 + r * sc + dy,
							 on ? 255 : 0);
			}
		}
		x += 6 * sc;
	}
}

/* 左上角两行: "#<seq>" / "t mm:ss.mmm" (相对首帧) */
static void draw_osd(unsigned char *y_plane, int stride, int W, int H)
{
	char l1[32], l2[32];
	unsigned el = g_osd_ms > 0 ? (unsigned)(g_osd_ms + 0.5) : 0;
	int sec = (int)(el / 1000), ms = (int)(el % 1000);
	int x, y;

	snprintf(l1, sizeof(l1), "#%u", g_osd_seq);
	snprintf(l2, sizeof(l2), "t %02d:%02d.%03d", sec / 60, sec % 60, ms);
	/* 先铺黑底再画白字, 保证任何背景上都可读(与 camd 同) */
	for (y = 2; y < 2 + 2 * 9 * 2 + 4; y++)
		for (x = 2; x < 2 + 14 * 12 + 4; x++)
			put_y_px(y_plane, stride, W, H, x, y, 0);
	draw_text(y_plane, stride, W, H, 4, 4, l1, 2);
	draw_text(y_plane, stride, W, H, 4, 4 + 16, l2, 2);
}

/* ---------------- MPP ---------------- */
static MppCtx g_ctx;
static MppApi *g_mpi;
static MppBufferGroup g_grp;
static MppBuffer g_fbuf;
static size_t g_fsize;

static int enc_init(int w, int h, int stride, int kbps, int gop)
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
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", stride);
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

		/* 不要在这里调 MPP_ENC_GET_HDR_SYNC: 这个 control 会把编码器
		 * 内部头标记为"已由 ctrl 交付"(added_by_ctrl), 于是首个 IDR
		 * 不再自动带 SPS/PPS; 用户侧若没接住(旧代码传 NULL), 头就丢
		 * 了, 要等第二个 IDR 才恢复(2026-10 板端实测)。
		 * EACH_IDR 模式下让编码器自己在首个 IDR 里带头即可: 启动后
		 * 第一帧(~70 ms)就能从码流扫描到参数集, 供 SDP/dump 使用。 */
		g_mpi->control(g_ctx, MPP_ENC_SET_HEADER_MODE, &hm);
	}
	mpp_buffer_group_get_internal(&g_grp, MPP_BUFFER_TYPE_ION);
	g_fsize = (size_t)stride * h * 3 / 2;
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
static int g_det_announced;	    /* 本轮连击是否已打过"检测触发" */
/* S1(事件治理,docs/37): 事件延长判定用 last_detect_ms,不用 streak ——
 * 检测偶尔漏一帧会清 streak,但"目标是否持续在场"应以最近一次命中时间
 * 加 post 窗口来判;streak 只保留触发门限语义。 */
static double g_last_detect_ms; /* 最近一次检测命中(ms,同 ring ts 基准) */
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

	{	/* 触发判定: 目标类 + 分数门限, 且必须连续 N 帧。
		 * P5: 只在状态变化时打印, 不再每帧一行 ——
		 *   ① 连击开始(streak 0->1)一行: 本次连击的首帧分数;
		 *   ② 本次连击首次达标(streak 达到 g_det_frames)一行:
		 *      "检测触发", 报达标帧的 streak 与分数。
		 * 预热期主循环会把 g_trigger 复位(见 main 里"预热未满"),
		 * 因此不能用 "!g_trigger" 判断是否打印 —— 那样预热期每帧都会
		 * 重开一次门限, A1 曾连刷 28 行(streak=3..30)。这里用
		 * g_det_announced 记住"本轮连击已报过", 预热复位 g_trigger
		 * 不会影响它; 置位仍用 s >= g_det_frames, 预热结束后哪怕
		 * streak 已经很大也能立刻触发。连击中断(streak 归零)或事件
		 * 完成时清 g_det_announced, 下一轮连击照常各打一行。 */
		int hit = 0, k, s;

		for (k = 0; k < nd; k++)
			if (boxes[k].cls == 0 && boxes[k].score >= g_det_score) {
				hit = 1;
				break;
			}
		if (hit) {
			s = ++g_streak;
			/* S1: 命中即刷新"目标在场"时刻(事件延长判定唯一依据);
			 * 与主循环 ts 同用 CLOCK_MONOTONIC, 基准一致 */
			g_last_detect_ms = now_ms(CLOCK_MONOTONIC);
			if (s == 1 && g_det_frames > 1)
				printf(">>> 检测连击开始 streak=1 score=%.3f "
				       "(阈值 %.2f, 需连续 %d 帧)\n",
				       (double)boxes[k].score, (double)g_det_score,
				       g_det_frames);
			if (s >= g_det_frames) {
				if (!g_det_announced) {
					g_det_announced = 1;
					printf(">>> 检测触发 streak=%d score=%.3f\n",
					       s, (double)boxes[k].score);
				}
				g_trigger = 1;
			}
		} else {
			g_streak = 0;
			g_det_announced = 0;
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

/* ---------------- S2: 流式事件写入(docs/37 §3.3) ----------------
 * 事件 = 临时文件(.part)打开后逐包 append;结束才 close+rename+JSON。
 * pre-roll 在事件开启时从 ring 取(自最近 IDR);之后 ring_push 单点 append。
 * 背压: 用户态 4MB 缓冲批量写, 不逐包 fsync; flush 失败/超时立即收尾当前
 * 文件并停录(write_error), 绝不阻塞采集(docs/37 §3.5)。
 * 注意: ring_push 在主循环线程内调用, writer 写失败只置标志, 文件收尾
 * 延迟到主循环的事件状态机里做(避免 IO 在 push 路径上拖采集)。 */
static FILE *g_ev_fp;			/* 当前事件 .part 文件 */
static char g_ev_path[512];		/* 最终名(不含 .part) */
static unsigned g_ev_start_seq;		/* 事件首包 seq(seq 去重用) */
static int g_ev_have_first;		/* 已写过首个包 */
static unsigned long g_ev_bytes;	/* 当前事件已写字节 */
static double g_ev_open_ms;		/* 事件开启时刻 */
static const unsigned EV_BUF_CAP = 4u << 20;
static int g_ev_werr;			/* 写失败标志(主循环收尾时清) */
static char g_out_base[512];		/* 输出前缀(main 里 -o 的值) */
static unsigned g_ev_last_seq;		/* 已写入 ring 内容的最大 seq */

static unsigned char ev_buf[4 << 20];	/* 用户态写缓冲(批量, 不逐包 fsync) */
static unsigned ev_bn;

static void ev_flush(void)
{
	if (ev_bn == 0)
		return;
	if (g_ev_fp && fwrite(ev_buf, 1, ev_bn, g_ev_fp) != ev_bn)
		g_ev_werr = 1;
	g_ev_bytes += ev_bn;
	ev_bn = 0;
}

static void evw_write(const unsigned char *d, int len, unsigned seq)
{

	/* seq 过滤: 开启时已从 ring 写到 g_ev_last_seq, 之前的包跳过 */
	if (seq <= g_ev_last_seq)
		return;
	g_ev_have_first = 1;

	if (ev_bn + (unsigned)len > EV_BUF_CAP)
		ev_flush();
	if (ev_bn + (unsigned)len <= EV_BUF_CAP) {
		memcpy(ev_buf + ev_bn, d, (size_t)len);
		ev_bn += (unsigned)len;
	} else {
		/* 单包超缓冲(理论不会): 直写 */
		if (g_ev_fp && fwrite(d, 1, (size_t)len, g_ev_fp) != (size_t)len)
			g_ev_werr = 1;
		g_ev_bytes += (unsigned)len;
	}
}

/* S2: 事件开启 —— 开 .part, 从 ring 写 pre-roll(自 IDR 到 ring 尾),
 * 并把 g_ev_last_seq 置为 ring 尾 seq; 之后 ring_push 的新包按 seq>last 过滤。 */

static struct rec *ring_at(int i);	/* 前向声明 */
static int evw_open(int pre_i)
{
	char path[560];
	int i;
	unsigned last = 0;

	snprintf(g_ev_path, sizeof(g_ev_path), "%s-%02u", g_out_base,
		 (unsigned)(g_events + 1));
	snprintf(path, sizeof(path), "%s.part", g_ev_path);
	g_ev_fp = fopen(path, "wb");
	if (!g_ev_fp) {
		fprintf(stderr, ">>> evw_open %s 失败: %s\n",
			path, strerror(errno));
		return -1;
	}
	g_ev_have_first = 0;
	g_ev_bytes = 0;
	g_ev_werr = 0;
	g_ev_open_ms = now_ms(CLOCK_MONOTONIC);
	/* pre-roll: pre_i(IDR) 到 ring 尾; 这些 seq 之后由 evw_write 过滤 */
	for (i = (pre_i >= 0 ? pre_i : 0); g_ev_fp && i < ring_cnt; i++) {
		struct rec *r = ring_at(i);
		if (fwrite(r->data, 1, (size_t)r->len, g_ev_fp) != (size_t)r->len) {
			g_ev_werr = 1;
			break;
		}
		g_ev_bytes += (unsigned)r->len;
		last = r->seq;
	}
	g_ev_last_seq = last;
	return 0;
}

/* S2: 事件收尾 —— flush+close+rename+JSON。close_reason 由调用方给。 */
static int evw_close(const char *reason, unsigned trig_seq, double trig_ts)
{
	char path[560], jpath[560];
	FILE *j;
	double now = now_ms(CLOCK_MONOTONIC);

	ev_flush();
	if (g_ev_fp && fclose(g_ev_fp) != 0)
		g_ev_werr = 1;
	g_ev_fp = NULL;
	snprintf(path, sizeof(path), "%s.part", g_ev_path);
	snprintf(jpath, sizeof(jpath), "%s.json", g_ev_path);
	if (rename(path, g_ev_path) != 0) {
		fprintf(stderr, ">>> rename %s 失败: %s\n",
			path, strerror(errno));
		return -1;
	}
	j = fopen(jpath, "w");
	if (j) {
		fprintf(j,
			"{\"file\":\"%s.h264\",\"trigger_seq\":%u,"
			"\"trigger_ts_ms\":%.1f,\"close_reason\":\"%s\","
			"\"bytes\":%lu,\"duration_ms\":%.1f,"
			"\"write_error\":%s}\n",
			g_ev_path, trig_seq, trig_ts, reason,
			g_ev_bytes, now - g_ev_open_ms,
			g_ev_werr ? "true" : "false");
		fclose(j);
	}
	g_events++;
	return 0;
}

static void ring_push(unsigned char *d, int len, unsigned seq, unsigned pkt,
		      int key, double cap_ms)
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
	r->seq = seq;	/* 源帧序号(可能多包重复) */
	r->pkt = pkt;
	r->key = key;
	r->ts_ms = now_ms(CLOCK_MONOTONIC) - t0_ms;
	r->abs_ms = now_ms(CLOCK_REALTIME);
	r->cap_ms = cap_ms;	/* 采集时刻(自启动); proc = ts_ms - cap_ms */
	ring_cnt++;
	g_pushed++;
	ring_bytes += (size_t)len;
	/* S2: 事件进行中, 新包实时 append 到当前事件(单点挂在 ring_push,
	 * 天然带 seq/key; 写失败只置 g_ev_werr, 由主循环收尾) */
	if (g_ev_fp && g_ev_have_first)
		evw_write(d, len, seq);
}

static struct rec *ring_at(int i)
{
	return &ring[(ring_head + i) % MAX_FRAMES];
}

/* 事件结束复位环: 必须释放帧数据, 否则每个事件都会漏掉一整环编码包副本 */
static void ring_clear(void)
{
	int i;

	for (i = 0; i < ring_cnt; i++)
		free(ring_at(i)->data);
	ring_head = 0;
	ring_cnt = 0;
	ring_bytes = 0;
	g_pushed = 0;
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

/* ---------------- 内置 RTSP 服务端(单客户端, 协议照搬 cam_rtsp.c) ----------------
 *
 * 服务端只保留一个客户端: 第二个连接 accept 后立即 close, 不影响第一个。
 * sender_thread 从投递环取出编码包后调用 rtsp_send_packet():
 *   - 没有 PLAY / 没有客户端时直接返回, 包照旧 free;
 *   - 发送路径非阻塞(MSG_DONTWAIT + O_NONBLOCK), 队列满时用 poll(POLLOUT)
 *     有界等待(单包最多 1s), 所以慢客户端反压不到采集/编码/录像;
 *   - 一包要么 4+len 字节全部进内核, 要么把连接判死; 短写就接着写完,
 *     绝不把"发了一半"的包留在流里(P10: 帧内截断会造成宏块损坏);
 *   - 一帧一旦开始发就必须发完, 任何失败都只能断开连接;
 *   - g_rtsp_lock 把 "RTP 数据" 与 "RTSP 响应" 串行化, PLAY 的 200 OK
 *     绝不会被 RTP 插队(interleaved 流里响应必须完整先到)。
 */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SIOCOUTQ
#define SIOCOUTQ 0x5411		/* Linux: 发送队列里尚未确认的字节数 */
#endif

#define RTSP_REQ_MAX 4096
#define RTSP_SDP_MAX 2048

/* 发送结果: 0=整包已发出, DROP=整包丢弃(客户端还在), FATAL=连接已坏 */
#define RTSP_TX_OK 0
#define RTSP_TX_DROP (-1)
#define RTSP_TX_FATAL (-2)
#define RTSP_SLOW_DROP_MAX 30	/* 连续丢 30 帧(约 2s@15fps)就踢人 */

/* --rtp-trace 的诊断块: 由 rtsp_send_interleaved() 填, 交给 trace 打印.
 * 不打印时整个结构体根本不构造(传 NULL), 热路径只多一次指针判空. */
enum {
	TX_WHY_OK = 0,		/* 整包(4+RTP)已全部进内核 */
	TX_WHY_PRECHECK,	/* P10 前的旧行为: 预检命中整包丢弃(仅旧 trace 可见) */
	TX_WHY_EAGAIN,		/* EAGAIN 且不再等待(保留) */
	TX_WHY_ERROR,		/* 其它错误 / 对端关闭 */
	TX_WHY_SHORT,		/* 短写且无法继续(保留) */
	TX_WHY_STALL,		/* POLLOUT 有界等待超时: 客户端不收 */
};
#define RTSP_SNDBUF_REQ (512 * 1024)	/* 内核按 wmem_max 封顶, 实际 >= 2*wmem_max */
#define RTSP_TX_POLL_MS 100		/* 单次等可写的最长时间 */
#define RTSP_TX_WAIT_MAX 10		/* 单包累计最多等 1s, 超时即判定客户端已死 */
struct txdiag {
	int why;
	int w;			/* sendmsg 实际收下的字节数 */
	int total;		/* 本包期望字节数(4 + RTP) */
	int err;		/* errno(0=无) */
	int outq;		/* SIOCOUTQ 采样(-1=未采) */
	int cap;		/* SO_SNDBUF 采样(-1=未采) */
};

/* --rtp-trace FILE: 每个 RTP 包一行, 只在 --rtsp 时打开; 默认 NULL */
static FILE *g_rtp_trace;
static const char *g_rtp_trace_path;
static char g_rtp_trace_buf[1 << 20];	/* trace 全缓冲: 每包只 memcpy, 不落盘 */
static unsigned g_rtp_trace_n;

/* 行缓冲 + atexit 收尾: SIGTERM 走正常 return 时也把最后几行落盘 */
static void rtp_trace_close(void)
{
	if (g_rtp_trace) {
		fflush(g_rtp_trace);
		fclose(g_rtp_trace);
		g_rtp_trace = NULL;
	}
}

static int g_rtsp_port;			/* 0=关闭, --rtsp PORT 开启 */
static int g_rtsp_lfd = -1;
static pthread_t g_rtsp_accept_tid;
static int g_rtsp_accept_started;
static volatile sig_atomic_t g_rtsp_stopping;

/* 唯一客户端的状态, 全部由 g_rtsp_lock 保护 */
static pthread_mutex_t g_rtsp_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_rtsp_cfd = -1;
static int g_rtsp_used;
static int g_rtsp_play;
static int g_rtsp_started;
static int g_rtsp_need_key;
static volatile int g_rtsp_gap;	/* pkb 丢过包: 重置发送游标, 等 IDR */
static int g_rtsp_ch;
static uint16_t g_rtsp_seq;
static uint32_t g_rtsp_ts;
static pthread_t g_rtsp_cli_tid;
static int g_rtsp_cli_started, g_rtsp_cli_joined;
static unsigned long g_rtsp_sent, g_rtsp_drop;	/* 累计: 发成的帧 / 丢掉的帧 */
static unsigned long g_rtsp_disc;		/* 对端主动关闭次数(不计入 drop) */
static unsigned long g_rtsp_drop_run;	/* 连续丢包数: 任何一帧发成就清零 */
static int g_rtsp_kick;			/* 本轮连接是被慢客户端策略踢掉的 */
static double g_rtsp_next_ms = 1000.0;	/* 下一条 rtsp: 聚合日志的时间(相对启动) */

/* SPS/PPS: DESCRIBE 的 sprop-parameter-sets 与关键帧补发都要用。
 * 编码线程、发送线程、RTSP 线程都会碰, 用独立的短锁保护, 不与发送锁嵌套阻塞 */
static pthread_mutex_t g_nal_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_sps[128], g_pps[128];
static int g_sps_len, g_pps_len;

/* 丢掉恢复前编码器里残留的包: 否则旧包会顶着恢复后的新 frame_seq 入环,
 * .json/OSD 的序号就和码流内容对不上了。最多清 64 个, 防驱动异常时死循环。 */
static void enc_drain_packets(void)
{
	MppPacket pkt = NULL;
	int guard = 0;

	while (g_mpi->encode_get_packet(g_ctx, &pkt) == MPP_OK && pkt &&
	       ++guard < 64) {
		mpp_packet_deinit(&pkt);
		pkt = NULL;
	}
}

/*
 * 有界恢复: 只重建 V4L2 采集侧, MPP 编码器与 RTSP 服务端原样保留。
 *
 * used = 本次故障链已经用掉的尝试次数: STREAMON 后一帧都没采到就又失败,
 * 说明设备还在抖, 继续用同一轮的剩余次数, 不要无限重开。
 * 返回新的已用次数(>=1), 全部失败返回 -1; 调用方打印最终失败并 return 2。
 */
static int link_recover(const char *dev, int w, int h, int used)
{
	int i;

	for (i = used; i < g_recover; i++) {
		int r;

		/* SIGTERM 后不再重开: 保证优雅退出的 2s 上限不被坏设备拖长 */
		if (g_stop)
			return -1;
		cap_release();
		r = cap_open(dev, w, h, 4);
		printf(">>> 恢复 第 %d/%d 次: 重开设备 %s\n", i + 1, g_recover,
		       r == 0 ? "成功" : "失败");
		fflush(stdout);
		if (r < 0)
			continue;
		enc_drain_packets();
		/* 恢复后立刻要一个 IDR: 录像环/事件切片从这里重新对齐; RTSP
		 * 发送端等 IDR, EACH_IDR 模式下参数集随该 IDR 一起补发 */
		g_mpi->control(g_ctx, MPP_ENC_SET_IDR_FRAME, NULL);
		pthread_mutex_lock(&g_rtsp_lock);
		g_rtsp_started = 0;
		g_rtsp_need_key = 1;
		pthread_mutex_unlock(&g_rtsp_lock);
		return i + 1;
	}
	return -1;
}

/* 从 Annex-B 缓冲解析 NAL, 记录 SPS/PPS(与 cam_rtsp.c 相同) */
static void rtsp_scan_nals(const uint8_t *d, int n)
{
	int i = 0;

	while (i + 3 < n) {
		int sc = 0, st, j, len;

		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
			sc = 3;
		else if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 &&
			 d[i + 2] == 0 && d[i + 3] == 1)
			sc = 4;
		if (!sc) {
			i++;
			continue;
		}
		st = i + sc;
		j = st;
		while (j + 3 < n) {
			if (d[j] == 0 && d[j + 1] == 0 &&
			    (d[j + 2] == 1 || (d[j + 2] == 0 && d[j + 3] == 1)))
				break;
			j++;
		}
		if (j + 3 >= n)
			j = n;
		len = j - st;
		if (len > 0) {
			int t = d[st] & 0x1f;

			if (t == 7 && len < (int)sizeof(g_sps)) {
				memcpy(g_sps, d + st, (size_t)len);
				g_sps_len = len;
			}
			if (t == 8 && len < (int)sizeof(g_pps)) {
				memcpy(g_pps, d + st, (size_t)len);
				g_pps_len = len;
			}
		}
		i = j;
	}
}

static int rtsp_has_nal(const uint8_t *d, int n, int type)
{
	int i;

	for (i = 0; i + 4 < n; i++)
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 &&
		    (d[i + 3] & 0x1f) == type)
			return 1;
	return 0;
}

/* --- TCP 短写: 非阻塞, 要么写完要么失败 ---
 * 响应(OPTIONS/DESCRIBE/SETUP/PLAY)本来就只有几百字节, 但慢客户端连
 * 这几百字节都可能不收; MSG_DONTWAIT 保证这里永远不会挂住 RTSP 线程。
 * 半截响应已经上线时只能返回失败: 调用方会断开该客户端, 不会带着错位的
 * interleaved 流继续跑。 */
static int rtsp_xwrite(int fd, const void *buf, size_t n)
{
	const char *p = (const char *)buf;
	size_t off = 0;

	while (off < n) {
		ssize_t w;

		if (g_rtsp_stopping)
			return -1;
		w = send(fd, p + off, n - off, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (w < 0) {
			if (errno == EINTR)	/* 一个字节都没发出去, 重试不等待 */
				continue;
			return -1;		/* EAGAIN/EPIPE/ECONNRESET: 失败 */
		}
		if (w == 0)
			return -1;		/* 对端关闭 */
		off += (size_t)w;
	}
	return 0;
}

/* 查内核发送队列: 还剩多少空间. 取不到(极老内核)返回 0, 交给 sendmsg 兜底 */
static int rtsp_snd_room(int fd, int *used, int *cap)
{
	int outq = 0, sndbuf = 0;
	socklen_t sl = sizeof(sndbuf);

	if (ioctl(fd, SIOCOUTQ, &outq) < 0)
		return 0;
	if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl) < 0)
		return 0;
	*used = outq;
	*cap = sndbuf;
	return 1;
}

/* 一行 trace: seq/ts/pt/mark/nal/fu/s/e/payload/send 是固定字段,
 * 后面 why/w/errno/outq 是"为什么没整包发出去"的诊断, 便于事后定责:
 *   why=ok        send=0, w=total         整包已进内核
 *   why=precheck  send=-1, w=0, errno=0   预检命中, 一个字节都没发
 *                (P10 修复前的旧行为, 保留字符串只为能读旧 trace)
 *   why=eagain    send=-1, w=0, errno=11  队列满且不再等待
 *   why=error     send=-2, w=已发字节, errno=e  EPIPE/ECONNRESET 等
 *   why=short     send=-2, w<total        短写, 只上线了一部分
 *   why=stall     send=-2, w=已发字节, errno=11  等 POLLOUT 超时(客户端不收) */
static void rtp_trace_line(uint16_t seq, uint32_t ts, int nal, int fu,
			   int sbit, int ebit, int marker, int payload,
			   int rc, const struct txdiag *dg)
{
	static const char * const why[] = { "ok", "precheck", "eagain",
					    "error", "short", "stall" };

	if (!g_rtp_trace)
		return;
	fprintf(g_rtp_trace,
		"seq=%u ts=%u pt=96 mark=%d nal=%d fu=%d s=%d e=%d payload=%d send=%d why=%s w=%d/%d errno=%d outq=%d/%d\n",
		(unsigned)seq, (unsigned)ts, marker, nal, fu, sbit, ebit,
		payload, rc, dg ? why[dg->why] : "?", dg ? dg->w : -1,
		dg ? dg->total : -1, dg ? dg->err : 0,
		dg ? dg->outq : -1, dg ? dg->cap : -1);
	/* 每 256 包落一次盘(约每 2s): 既不进热路径, 也不至于丢太多证据 */
	if (!(++g_rtp_trace_n & 0xff))
		fflush(g_rtp_trace);
}

/*
 * 发一个 interleaved 包($ + ch + 2 字节长度 + RTP). 整包语义(P10 重写):
 *
 * P10 根因(已用 --rtp-trace 证实): 旧实现在每个 RTP 包发送前做
 * "SIOCOUTQ + total > SO_SNDBUF - SO_SNDBUF/8" 预检, 装不下就 return
 * RTSP_TX_DROP(一个字节都不发), 而上层 rtsp_send_frame() 又在第一片失败时
 * 放弃该帧剩余分片 —— 一个 68~155KB 的 IDR 在内核发送队列写满时被从
 * 中间截断: slice 尾部缺失、FU-A 的 E=1 和 RTP marker 都没发出, 解码器
 * 就在这一帧看到 "corrupted macroblock / error while decoding MB"。
 *
 * 现在的规则:
 *   - 不做包级丢弃: 一个 RTP 包要么整包进内核, 要么把连接判死(FATAL),
 *     绝不会留下"发了前缀"的半个包;
 *   - EAGAIN 用 poll(POLLOUT) 有界等待: 单包累计最多 RTSP_TX_WAIT_MAX×
 *     RTSP_TX_POLL_MS = 1s, 有进展就继续写, 超时才 FATAL;
 *   - EPIPE/ECONNRESET/对端关闭/等待超时 → FATAL, 上层 shutdown 该连接;
 *     "半个访问单元留在线上" 比断开严重得多, 所以帧内失败只能断连。
 * 所有写都发生在 g_rtsp_lock 里(单写者), 帧内不可能有并发写。
 */
static int rtsp_send_interleaved(int fd, int ch, const uint8_t *rtp, int len,
				 struct txdiag *dg)
{
	/* 4 + RTP: RTP 头 12 + 负载 <= 1400, 连续缓冲区让短写可以简单续写 */
	uint8_t buf[4 + 12 + 1400];
	int total, off = 0, waited = 0, outq = 0, sndbuf = 0;

	if (dg) {
		dg->why = TX_WHY_OK;
		dg->w = 0;
		dg->total = 4 + len;
		dg->err = 0;
		dg->outq = -1;
		dg->cap = -1;
	}
	if (len <= 12 || len > 12 + 1400)
		return RTSP_TX_FATAL;	/* 调用方 bug: 不可能是合法 RTP 包 */
	total = 4 + len;
	buf[0] = '$';
	buf[1] = (uint8_t)ch;
	buf[2] = (uint8_t)(len >> 8);
	buf[3] = (uint8_t)(len & 0xff);
	memcpy(buf + 4, rtp, (size_t)len);
	/* 只记录队列/缓冲状态供 trace 定责, 不再据此丢包 */
	if (rtsp_snd_room(fd, &outq, &sndbuf) && dg) {
		dg->outq = outq;
		dg->cap = sndbuf;
	}

	while (off < total) {
		ssize_t w;

		if (g_rtsp_stopping) {
			errno = EINTR;
			if (dg) {
				dg->why = TX_WHY_ERROR;
				dg->w = off;
				dg->err = errno;
			}
			return RTSP_TX_FATAL;
		}
		w = send(fd, buf + off, (size_t)(total - off),
			 MSG_NOSIGNAL | MSG_DONTWAIT);
		if (w > 0) {
			off += (int)w;	/* 短写: 继续把剩下的字节发完 */
			continue;
		}
		if (w == 0) {		/* 对端关闭 */
			errno = EPIPE;
			if (dg) {
				dg->why = TX_WHY_ERROR;
				dg->w = off;
				dg->err = errno;
			}
			return RTSP_TX_FATAL;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			if (dg) {
				dg->why = TX_WHY_ERROR;
				dg->w = off;
				dg->err = errno;
			}
			return RTSP_TX_FATAL;	/* EPIPE/ECONNRESET/其它 */
		}
		/* 队列满: 有界等待可写, 绝不把 EAGAIN 当成已发送 */
		if (++waited > RTSP_TX_WAIT_MAX) {
			if (dg) {
				dg->why = TX_WHY_STALL;
				dg->w = off;
				dg->err = errno;
			}
			return RTSP_TX_FATAL;	/* 1s 写不进一个包: 客户端已死 */
		}
		{
			struct pollfd pf;
			int pr;

			pf.fd = fd;
			pf.events = POLLOUT;
			pf.revents = 0;
			pr = poll(&pf, 1, RTSP_TX_POLL_MS);
			if (pr < 0) {
				if (errno == EINTR)
					continue;
				if (dg) {
					dg->why = TX_WHY_ERROR;
					dg->w = off;
					dg->err = errno;
				}
				return RTSP_TX_FATAL;
			}
			if (pr > 0 && (pf.revents &
				       (POLLERR | POLLHUP | POLLNVAL))) {
				errno = EPIPE;
				if (dg) {
					dg->why = TX_WHY_ERROR;
					dg->w = off;
					dg->err = EPIPE;
				}
				return RTSP_TX_FATAL;
			}
		}
	}
	if (dg)
		dg->w = off;
	return RTSP_TX_OK;
}

static int rtsp_send_nal(int fd, int ch, const uint8_t *nal, int len,
			 uint32_t ts, uint16_t *seq, int marker,
			 struct txdiag *dg)
{
	const int MAXP = 1400;
	int rc;

	if (len <= MAXP) {
		uint8_t pkt[12 + MAXP];
		uint16_t sq = *seq;

		memset(pkt, 0, 12);
		pkt[0] = 0x80;
		pkt[1] = 96 | (marker ? 0x80 : 0);
		pkt[2] = (uint8_t)(sq >> 8);
		pkt[3] = (uint8_t)sq;
		pkt[4] = (uint8_t)(ts >> 24);
		pkt[5] = (uint8_t)(ts >> 16);
		pkt[6] = (uint8_t)(ts >> 8);
		pkt[7] = (uint8_t)ts;
		memcpy(pkt + 12, nal, (size_t)len);
		(*seq)++;
		rc = rtsp_send_interleaved(fd, ch, pkt, 12 + len, dg);
		rtp_trace_line(sq, ts, nal[0] & 0x1f, 0, 0, 0, marker,
			       len, rc, dg);
		return rc;
	}
	{
		int off = 1, first = 1;

		while (off < len) {
			int n = len - off;
			int end;
			uint8_t pkt[12 + MAXP];
			uint16_t sq = *seq;

			if (n > MAXP - 2)
				n = MAXP - 2;
			end = (off + n >= len);
			memset(pkt, 0, 12);
			pkt[0] = 0x80;
			pkt[1] = 96 | ((end && marker) ? 0x80 : 0);
			pkt[2] = (uint8_t)(sq >> 8);
			pkt[3] = (uint8_t)sq;
			pkt[4] = (uint8_t)(ts >> 24);
			pkt[5] = (uint8_t)(ts >> 16);
			pkt[6] = (uint8_t)(ts >> 8);
			pkt[7] = (uint8_t)ts;
			pkt[12] = (uint8_t)((nal[0] & 0xe0) | 28);	/* FU indicator */
			pkt[13] = (uint8_t)((nal[0] & 0x1f) |
					    (first ? 0x80 : 0) | (end ? 0x40 : 0));
			memcpy(pkt + 14, nal + off, (size_t)n);
			(*seq)++;
			/* 一片发不出去只能是连接坏了(整包要么全进内核, 要么
			 * 有界等待后 FATAL): 返回 FATAL 让上层立刻 shutdown,
			 * 绝不把缺了后半段的 FU-A 留在线上 */
			rc = rtsp_send_interleaved(fd, ch, pkt, 14 + n, dg);
			rtp_trace_line(sq, ts, nal[0] & 0x1f, 1, first,
				       end, end && marker, n + 2, rc, dg);
			if (rc != RTSP_TX_OK)
				return rc;
			off += n;
			first = 0;
		}
	}
	return RTSP_TX_OK;
}

/* 把整帧(Annex-B, 多个 NAL)按 RTP 发出; 最后一个包打 marker。
 * P10 起: 帧内任何一个包失败都返回 FATAL(连接作废), 不再"放弃整帧"——
 * 已发出的前缀会构成一个缺 slice 尾部的访问单元, 解码器只能报宏块错误。
 * 只有 RTSP_TX_OK / RTSP_TX_FATAL 两种返回值会从这里出现。 */
static int rtsp_send_frame(int fd, int ch, const uint8_t *d, int n, int key,
			   uint32_t ts, uint16_t *seq, struct txdiag *dg)
{
	struct { const uint8_t *p; int len; } nal[64];
	int nn = 0, i = 0, k, rc;

	while (i + 3 < n && nn < 64) {
		int sc = 0, st, j, len;

		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
			sc = 3;
		else if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 &&
			 d[i + 2] == 0 && d[i + 3] == 1)
			sc = 4;
		if (!sc) {
			i++;
			continue;
		}
		st = i + sc;
		j = st;
		while (j + 3 < n) {
			if (d[j] == 0 && d[j + 1] == 0 &&
			    (d[j + 2] == 1 || (d[j + 2] == 0 && d[j + 3] == 1)))
				break;
			j++;
		}
		if (j + 3 >= n)
			j = n;
		len = j - st;
		if (len > 0) {
			nal[nn].p = d + st;
			nal[nn].len = len;
			nn++;
		}
		i = j;
	}
	if (!nn)
		return 0;

	/* 关键帧缺 SPS/PPS 才补, 本编码器 EACH_IDR 通常已自带, 避免重复 */
	if (key) {
		uint8_t sps[sizeof(g_sps)], pps[sizeof(g_pps)];
		int sl, pl;

		pthread_mutex_lock(&g_nal_lock);
		sl = g_sps_len;
		pl = g_pps_len;
		if (sl > 0)
			memcpy(sps, g_sps, (size_t)sl);
		if (pl > 0)
			memcpy(pps, g_pps, (size_t)pl);
		pthread_mutex_unlock(&g_nal_lock);

		if (sl > 0 && !rtsp_has_nal(d, n, 7)) {
			rc = rtsp_send_nal(fd, ch, sps, sl, ts, seq, 0, dg);
			if (rc != RTSP_TX_OK)
				return rc;
		}
		if (pl > 0 && !rtsp_has_nal(d, n, 8)) {
			rc = rtsp_send_nal(fd, ch, pps, pl, ts, seq, 0, dg);
			if (rc != RTSP_TX_OK)
				return rc;
		}
	}
	for (k = 0; k < nn; k++) {
		rc = rtsp_send_nal(fd, ch, nal[k].p, nal[k].len, ts, seq,
				   k == nn - 1, dg);
		if (rc != RTSP_TX_OK)
			return rc;
	}
	return RTSP_TX_OK;
}

/* sender_thread 的唯一网络出口: 取一个包, 有 PLAY 中的客户端就发, 否则丢 */
static void rtsp_send_packet(const unsigned char *d, int n, int key,
			     uint32_t ts90)
{
	struct txdiag dg;
	int rc;

	pthread_mutex_lock(&g_rtsp_lock);
	if (!g_rtsp_play || g_rtsp_cfd < 0)
		goto out;
	if (g_rtsp_gap) {
		/* 投递环丢过包: 中间帧已不可解, 从下一个 IDR 重新起步 */
		g_rtsp_gap = 0;
		g_rtsp_need_key = 1;
		g_rtsp_started = 0;
	}
	if (!g_rtsp_started) {
		/* 中途接入先等 IDR, 否则首帧是 P 帧, 播放器解不出来 */
		if (!key)
			goto out;
		g_rtsp_started = 1;
	}
	if (g_rtsp_need_key) {
		if (!key)
			goto out;
		g_rtsp_need_key = 0;
	}
	if (!g_sps_len || !g_pps_len) {
		pthread_mutex_lock(&g_nal_lock);
		if (!g_sps_len || !g_pps_len)
			rtsp_scan_nals(d, n);
		pthread_mutex_unlock(&g_nal_lock);
	}
	g_rtsp_ts = ts90;
	rc = rtsp_send_frame(g_rtsp_cfd, g_rtsp_ch, d, n, key, g_rtsp_ts,
			     &g_rtsp_seq, g_rtp_trace ? &dg : NULL);
	if (rc == RTSP_TX_OK) {
		g_rtsp_sent++;
		g_rtsp_drop_run = 0;
		goto out;
	}
	/* 失败: 连接作废, 立刻 shutdown; 已发出的帧前缀随连接一起丢, 绝不让
	 * 半个访问单元继续流下去(编码/录像完全不受影响) */
	if (rc == RTSP_TX_DROP) {
		/* P10 起帧内不再返回 DROP, 这里只作防御保留 */
		g_rtsp_drop++;
		g_rtsp_kick = 1;
		g_rtsp_play = 0;
		if (g_rtsp_cfd >= 0)
			shutdown(g_rtsp_cfd, SHUT_RDWR);
	} else {
		/* FATAL: EPIPE/ECONNRESET 是"对端关掉了", 不是我们丢包 —— 计入
		 * disc。EAGAIN(POLLOUT 等 1s 仍写不进)是客户端不收, 计入 drop。
		 * errno 在锁内立即取, 打印后失效。 */
		int e = errno;

		if (e == EPIPE || e == ECONNRESET) {
			g_rtsp_disc++;
			printf("rtsp: 客户端已关闭(errno=%d %s), 断开\n",
			       e, strerror(e));
		} else if (e == EAGAIN || e == EWOULDBLOCK) {
			g_rtsp_drop++;
			printf("rtsp: 客户端不收(等待可写超时, errno=%d), 断开客户端\n",
			       e);
		} else {
			g_rtsp_drop++;
			printf("rtsp: 发送失败(errno=%d %s), 断开客户端\n",
			       e, strerror(e));
		}
		fflush(stdout);
		g_rtsp_play = 0;
		if (g_rtsp_cfd >= 0)
			shutdown(g_rtsp_cfd, SHUT_RDWR);
	}
out:
	pthread_mutex_unlock(&g_rtsp_lock);
}

/* 进程当前打开的 fd 数(/proc/self/fd): 盯 accept/close 有没有泄漏 */
static int rtsp_count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n - 1;	/* 减掉 opendir 自己占的那个 */
}

/* 每秒一行的聚合日志(不是每包一行): 客户端数 / 已发帧 / 丢帧 / fd 数 */
static void rtsp_stats_report(void)
{
	unsigned long sent, drop, disc;
	int clients, fds;

	pthread_mutex_lock(&g_rtsp_lock);
	clients = (g_rtsp_used && g_rtsp_cfd >= 0) ? 1 : 0;
	sent = g_rtsp_sent;
	drop = g_rtsp_drop;
	disc = g_rtsp_disc;
	pthread_mutex_unlock(&g_rtsp_lock);
	fds = rtsp_count_fds();
	printf("rtsp: clients=%d sent=%lu drop=%lu disc=%lu fds=%d\n",
	       clients, sent, drop, disc, fds);
	fflush(stdout);
}

/* profile-level-id 必须与真实 SPS 一致, 否则播放器按错误的 level/profile
 * 配置解码器。SPS = NAL 头(1) + profile_idc + constraint_flags + level_idc,
 * 直接取 SPS[1..3] 拼成 6 位十六进制; 参数集没就绪或字节非法时回退 640028。 */
static void rtsp_profile_level(const uint8_t *sps, int n, char *out)
{
	if (n >= 4 && sps[1] != 0 && sps[3] != 0) {
		snprintf(out, 7, "%02x%02x%02x", sps[1], sps[2], sps[3]);
		return;
	}
	memcpy(out, "640028", 7);	/* 6 字符 + NUL */
}

static const char *rtsp_b64(const uint8_t *d, int n, char *out)
{
	static const char T[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, o = 0;

	for (i = 0; i + 2 < n; i += 3) {
		out[o++] = T[d[i] >> 2];
		out[o++] = T[((d[i] & 3) << 4) | (d[i + 1] >> 4)];
		out[o++] = T[((d[i + 1] & 15) << 2) | (d[i + 2] >> 6)];
		out[o++] = T[d[i + 2] & 63];
	}
	if (i < n) {
		out[o++] = T[d[i] >> 2];
		if (i + 1 < n) {
			out[o++] = T[((d[i] & 3) << 4) | (d[i + 1] >> 4)];
			out[o++] = T[(d[i + 1] & 15) << 2];
		} else {
			out[o++] = T[(d[i] & 3) << 4];
			out[o++] = '=';
		}
		out[o++] = '=';
	}
	out[o] = 0;
	return out;
}

/*
 * 读一个完整 RTSP 请求. 返回 >0=请求长度, 0=超时(继续等), -1=断开/退出.
 * 200ms poll 让线程能看见 g_rtsp_stopping; 客户端发来的 interleaved RTCP
 * 包在缓冲里直接跳过, 只把 RTSP 请求交给上层.
 */
static int rtsp_read_req(int fd, char *buf, int *plen, int cap)
{
	for (;;) {
		int len = *plen;
		char *p;

		if (g_rtsp_stopping)
			return -1;
		while (len >= 4 && buf[0] == '$') {
			int pl = ((unsigned char)buf[2] << 8) |
				 (unsigned char)buf[3];

			if (len < 4 + pl)
				break;
			memmove(buf, buf + 4 + pl, (size_t)(len - 4 - pl));
			len -= 4 + pl;
		}
		*plen = len;
		buf[len] = 0;
		p = memmem(buf, (size_t)len, "\r\n\r\n", 4);
		if (p) {
			p[4] = 0;
			*plen = 0;
			return (int)(p - buf) + 4;
		}
		if (len >= cap - 1) {
			*plen = 0;
			return -1;	/* 请求头过长: 不是合法 RTSP */
		}
		{
			struct pollfd pf;
			ssize_t r;
			int pr;

			pf.fd = fd;
			pf.events = POLLIN;
			pf.revents = 0;
			pr = poll(&pf, 1, 200);
			if (pr < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (pr == 0)
				continue;
			r = recv(fd, buf + len, (size_t)(cap - 1 - len), 0);
			if (r > 0) {
				len += (int)r;
				*plen = len;
				continue;
			}
			if (r == 0)
				return -1;
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -1;
		}
	}
}

static void *rtsp_client_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;
	char req[RTSP_REQ_MAX];
	int reqlen = 0, cseq = 0, kicked;
	char session[32] = "12345678";

	while (!g_rtsp_stopping) {
		char out[RTSP_SDP_MAX + 512];
		char sdp[RTSP_SDP_MAX];
		char b64s[512], b64p[512];
		char *p;
		int n = rtsp_read_req(fd, req, &reqlen, (int)sizeof(req));

		if (n < 0)
			break;
		if (n == 0)
			continue;
		p = strstr(req, "CSeq:");
		if (p)
			sscanf(p + 5, "%d", &cseq);

		if (!strncmp(req, "OPTIONS", 7)) {
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
				 "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n",
				 cseq);
			if (rtsp_xwrite(fd, out, strlen(out)) < 0)
				break;
		} else if (!strncmp(req, "DESCRIBE", 8)) {
			uint8_t sps[sizeof(g_sps)], pps[sizeof(g_pps)];
			char plid[8];
			int rc, sl, pl, waited;

			/* 参数集随首个 IDR 才出现(启动后约 70 ms): 有界等待,
			 * 让与设备同时启动的客户端也能拿到 SDP, 而不是 503。
			 * 最多等 1000 ms; 进程退出/客户端断开时立即放弃。 */
			for (waited = 0; waited < 1000; waited += 10) {
				int ready;

				pthread_mutex_lock(&g_nal_lock);
				ready = g_sps_len > 0 && g_pps_len > 0;
				pthread_mutex_unlock(&g_nal_lock);
				if (ready || g_rtsp_stopping || g_stop)
					break;
				usleep(10 * 1000);
			}

			/* 参数集可能在首帧 IDR 才出现: 拷贝快照, 不在发送锁里做 IO */
			pthread_mutex_lock(&g_nal_lock);
			sl = g_sps_len;
			pl = g_pps_len;
			if (sl > 0)
				memcpy(sps, g_sps, (size_t)sl);
			if (pl > 0)
				memcpy(pps, g_pps, (size_t)pl);
			pthread_mutex_unlock(&g_nal_lock);

			rtsp_profile_level(sps, sl, plid);
			pthread_mutex_lock(&g_rtsp_lock);
			if (sl > 0 && pl > 0 && g_rtsp_cfd == fd) {
				rtsp_b64(sps, sl, b64s);
				rtsp_b64(pps, pl, b64p);
				snprintf(sdp, sizeof(sdp),
					 "v=0\r\n"
					 "o=- 0 0 IN IP4 127.0.0.1\r\n"
					 "s=RK3568 Camera\r\n"
					 "c=IN IP4 0.0.0.0\r\n"
					 "t=0 0\r\n"
					 "a=control:*\r\n"
					 "m=video 0 RTP/AVP 96\r\n"
					 "a=rtpmap:96 H264/90000\r\n"
					 "a=fmtp:96 packetization-mode=1;profile-level-id=%s;"
					 "sprop-parameter-sets=%s,%s\r\n"
					 "a=control:trackID=0\r\n",
					 plid, b64s, b64p);
				snprintf(out, sizeof(out),
					 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
					 "Content-Base: rtsp://127.0.0.1:%d/live/\r\n"
					 "Content-Type: application/sdp\r\n"
					 "Content-Length: %d\r\n\r\n%s",
					 cseq, g_rtsp_port,
					 (int)strlen(sdp), sdp);
			} else {
				snprintf(out, sizeof(out),
					 "RTSP/1.0 503 Service Unavailable\r\nCSeq: %d\r\n\r\n",
					 cseq);
			}
			rc = rtsp_xwrite(fd, out, strlen(out));
			pthread_mutex_unlock(&g_rtsp_lock);
			if (rc < 0)
				break;
		} else if (!strncmp(req, "SETUP", 5)) {
			char *tr = strstr(req, "interleaved=");
			int ch_rtp = 0, ch_rtcp = 1, rc;

			if (tr)
				sscanf(tr + 12, "%d-%d", &ch_rtp, &ch_rtcp);
			pthread_mutex_lock(&g_rtsp_lock);
			if (g_rtsp_cfd == fd)
				g_rtsp_ch = ch_rtp;
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
				 "Session: %s\r\n"
				 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=12345678\r\n\r\n",
				 cseq, session, ch_rtp, ch_rtcp);
			rc = rtsp_xwrite(fd, out, strlen(out));
			pthread_mutex_unlock(&g_rtsp_lock);
			if (rc < 0)
				break;
		} else if (!strncmp(req, "PLAY", 4)) {
			int rc;

			/* 200 OK 必须完整先到, 之后才允许 RTP 上路 */
			pthread_mutex_lock(&g_rtsp_lock);
			if (g_rtsp_cfd != fd) {
				pthread_mutex_unlock(&g_rtsp_lock);
				break;
			}
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n"
				 "Range: npt=0.000-\r\n"
				 "RTP-Info: url=rtsp://127.0.0.1:%d/live/trackID=0;seq=%u;rtptime=%u\r\n\r\n",
				 cseq, session, g_rtsp_port,
				 (unsigned)g_rtsp_seq, (unsigned)g_rtsp_ts);
			rc = rtsp_xwrite(fd, out, strlen(out));
			if (rc == 0) {
				g_rtsp_play = 1;
				g_rtsp_started = 0;
				g_rtsp_need_key = 1;
				g_rtsp_gap = 0;
				g_rtsp_drop_run = 0;
			}
			pthread_mutex_unlock(&g_rtsp_lock);
			if (rc < 0)
				break;
			printf("rtsp: 客户端开始播放\n");
			fflush(stdout);
		} else if (!strncmp(req, "TEARDOWN", 8)) {
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseq);
			rtsp_xwrite(fd, out, strlen(out));
			break;
		} else {
			/* GET_PARAMETER 及其它: 统一回 200, 保持会话不死 */
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseq);
			if (rtsp_xwrite(fd, out, strlen(out)) < 0)
				break;
		}
	}

	/* 先让发送线程停, 再 shutdown 唤醒可能阻塞在 sendmsg 的发送线程 */
	pthread_mutex_lock(&g_rtsp_lock);
	g_rtsp_play = 0;
	kicked = g_rtsp_kick;
	g_rtsp_kick = 0;
	if (g_rtsp_cfd == fd) {
		g_rtsp_cfd = -1;
		g_rtsp_used = 0;
	}
	shutdown(fd, SHUT_RDWR);
	pthread_mutex_unlock(&g_rtsp_lock);
	close(fd);		/* fd 的生命周期只归本线程: 发送线程只 shutdown */
	printf("rtsp: 客户端断开%s\n", kicked ? " (slow client)" : "");
	fflush(stdout);
	return NULL;
}

static void *rtsp_accept_thread(void *arg)
{
	(void)arg;
	while (!g_rtsp_stopping && !g_stop) {
		struct pollfd pf;
		int pr, fd, one = 1, busy;

		pf.fd = g_rtsp_lfd;
		pf.events = POLLIN;
		pf.revents = 0;
		pr = poll(&pf, 1, 200);
		if (pr <= 0) {
			if (pr < 0 && errno != EINTR)
				break;
			continue;
		}
		fd = accept(g_rtsp_lfd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR || errno == EAGAIN ||
			    errno == EWOULDBLOCK)
				continue;
			break;
		}
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		{
			/* P10: 1080p@4Mbps 的 IDR 有 68~155KB, 而默认 SO_SNDBUF
			 * 只有 16KB(内核加倍 32KB) —— 一帧 IDR 必然把队列写满,
			 * 旧实现就在这里把帧截断。把发送缓冲要到位(内核按
			 * wmem_max 封顶后加倍, 实测 416KB), 单个 IDR 就能整体
			 * 进内核, 正常网络下根本不会出现 EAGAIN。 */
			int want = RTSP_SNDBUF_REQ, got = 0;
			socklen_t gl = sizeof(got);

			setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
			if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &got, &gl) == 0) {
				printf("rtsp: SO_SNDBUF=%d (请求 %d, wmem_max 封顶)\n",
				       got, want);
				fflush(stdout);
			}
		}
		{
			/* 发送全链路非阻塞: 慢客户端只会让包被丢掉, 绝不会
			 * 让 RTSP 线程卡在 send 上(原来的 SO_SNDTIMEO 2s
			 * 仍然会阻塞发送线程, 已删除) */
			int fl = fcntl(fd, F_GETFL, 0);

			if (fl >= 0)
				fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		}

		pthread_mutex_lock(&g_rtsp_lock);
		busy = g_rtsp_used;
		pthread_mutex_unlock(&g_rtsp_lock);
		if (busy) {
			close(fd);
			printf("rtsp: reject extra client\n");
			fflush(stdout);
			continue;
		}
		/* 槽位已空: 上一个客户端线程正在收尾, 先 join 回收栈 */
		if (g_rtsp_cli_started && !g_rtsp_cli_joined) {
			pthread_join(g_rtsp_cli_tid, NULL);
			g_rtsp_cli_joined = 1;
		}

		pthread_mutex_lock(&g_rtsp_lock);
		g_rtsp_used = 1;
		g_rtsp_cfd = fd;
		g_rtsp_play = 0;
		g_rtsp_started = 0;
		g_rtsp_need_key = 1;
		g_rtsp_gap = 0;
		g_rtsp_ch = 0;
		g_rtsp_seq = 0;
		g_rtsp_ts = 0;
		g_rtsp_kick = 0;
		g_rtsp_drop_run = 0;
		pthread_mutex_unlock(&g_rtsp_lock);

		if (pthread_create(&g_rtsp_cli_tid, NULL, rtsp_client_thread,
				   (void *)(intptr_t)fd) != 0) {
			pthread_mutex_lock(&g_rtsp_lock);
			if (g_rtsp_cfd == fd) {
				g_rtsp_cfd = -1;
				g_rtsp_used = 0;
			}
			pthread_mutex_unlock(&g_rtsp_lock);
			close(fd);
			continue;
		}
		g_rtsp_cli_started = 1;
		g_rtsp_cli_joined = 0;
		printf("rtsp: 客户端接入\n");
		fflush(stdout);
	}
	return NULL;
}

static int rtsp_start(int port)
{
	struct sockaddr_in sa;
	int one = 1;

	g_rtsp_lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_rtsp_lfd < 0) {
		perror("rtsp socket");
		return -1;
	}
	setsockopt(g_rtsp_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(g_rtsp_lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("rtsp bind");
		close(g_rtsp_lfd);
		g_rtsp_lfd = -1;
		return -1;
	}
	if (listen(g_rtsp_lfd, 8) < 0) {
		perror("rtsp listen");
		close(g_rtsp_lfd);
		g_rtsp_lfd = -1;
		return -1;
	}

	g_rtsp_stopping = 0;
	if (pthread_create(&g_rtsp_accept_tid, NULL, rtsp_accept_thread,
			   NULL) != 0) {
		close(g_rtsp_lfd);
		g_rtsp_lfd = -1;
		return -1;
	}
	g_rtsp_accept_started = 1;
	printf(">>> RTSP 监听 :%d (单客户端模式)\n", port);
	fflush(stdout);
	return 0;
}

/* 退出: 先让 accept/客户端线程退, 再关监听 fd, 保证没有线程还在用它 */
static void rtsp_stop(void)
{
	if (!g_rtsp_accept_started && g_rtsp_lfd < 0)
		return;
	g_rtsp_stopping = 1;
	if (g_rtsp_accept_started) {
		pthread_join(g_rtsp_accept_tid, NULL);
		g_rtsp_accept_started = 0;
	}
	pthread_mutex_lock(&g_rtsp_lock);
	g_rtsp_play = 0;
	if (g_rtsp_cfd >= 0)
		shutdown(g_rtsp_cfd, SHUT_RDWR);
	pthread_mutex_unlock(&g_rtsp_lock);
	if (g_rtsp_cli_started && !g_rtsp_cli_joined) {
		pthread_join(g_rtsp_cli_tid, NULL);
		g_rtsp_cli_joined = 1;
	}
	if (g_rtsp_lfd >= 0) {
		close(g_rtsp_lfd);
		g_rtsp_lfd = -1;
	}
}

/* ---------------- RTSP 投递环(编码线程 -> 发送线程) ----------------
 *
 *   - 编码线程对每个编码包 malloc+memcpy 一份投递, 环满丢最旧,
 *     绝不等待消费者; 发送慢只会丢自己的帧, 不会反压采集/编码/录像;
 *   - 发送线程取包后交给 rtsp_send_packet(): 没有客户端/没 PLAY 时
 *     直接返回, 有客户端才按单客户端游标发 RTP; 之后一律 free;
 *   - 停止只认 g_pkb_stop_local(由 pkb_set_stop 在锁内置位), 不直接读
 *     信号 handler 的 g_stop, 避免无锁跨线程可见性问题;
 *   - 录像环/OSD/检测路径不经过这里。默认深度 8; --rtsp 0 时发送线程
 *     只做取包/free, 行为与改动前一致。
 */
#define PKB_MAX 64		/* --rtsp-ring 的硬上限 */
struct pktbuf {
	unsigned char *data;
	int len;
	unsigned seq;
	unsigned char key;
	uint32_t ts90;		/* 90kHz RTP 时间戳, 编码线程按源帧时间算好 */
};

static struct pktbuf g_pkb[PKB_MAX];
static int g_pkb_depth = 8;	/* --rtsp-ring N, 默认 8 */
static int g_pkb_head, g_pkb_cnt;
static int g_pkb_stop_local;	/* 受 g_pkb_lock 保护: sender 的唯一停止条件 */
static unsigned long g_pkb_in, g_pkb_drop, g_pkb_out;
static pthread_mutex_t g_pkb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pkb_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_pkb_tid;
static int g_pkb_started;
static double g_pkb_next_ms = 5000.0;	/* 下一次打印 in/drop 的时间(相对启动) */

/* 编码线程: 只做一次拷贝入环, 环满丢最旧。计数与队列同一把锁, 天然原子。 */
static void pkb_push(const unsigned char *src, int len, unsigned seq, int key,
		     double ts_ms)
{
	struct pktbuf *p;
	unsigned char *copy, *victim = NULL;

	if (!g_pkb_started || len <= 0)
		return;
	copy = malloc((size_t)len);
	if (!copy) {
		pthread_mutex_lock(&g_pkb_lock);
		g_pkb_drop++;
		g_rtsp_gap = 1;
		pthread_mutex_unlock(&g_pkb_lock);
		return;
	}
	memcpy(copy, src, (size_t)len);
	/* SDP 需要 SPS/PPS; 编码头没拿到时直接从码流里补(找到后不再扫) */
	if (g_rtsp_port > 0) {
		pthread_mutex_lock(&g_nal_lock);
		if (!g_sps_len || !g_pps_len)
			rtsp_scan_nals(copy, len);
		pthread_mutex_unlock(&g_nal_lock);
	}

	pthread_mutex_lock(&g_pkb_lock);
	if (g_pkb_cnt == g_pkb_depth) {
		/* 环满: 摘掉最旧的一个, free 放到锁外做, 缩短编码线程持锁时间 */
		p = &g_pkb[g_pkb_head];
		victim = p->data;
		p->data = NULL;
		g_pkb_head = (g_pkb_head + 1) % g_pkb_depth;
		g_pkb_cnt--;
		g_pkb_drop++;
		g_rtsp_gap = 1;	/* 丢过包: 发送端从下一个 IDR 重新起步 */
	}
	p = &g_pkb[(g_pkb_head + g_pkb_cnt) % g_pkb_depth];
	p->data = copy;
	p->len = len;
	p->seq = seq;
	p->key = (unsigned char)(key ? 1 : 0);
	p->ts90 = (uint32_t)(ts_ms * 90.0);
	g_pkb_cnt++;
	g_pkb_in++;
	pthread_cond_signal(&g_pkb_cond);
	pthread_mutex_unlock(&g_pkb_lock);

	free(victim);		/* 锁外释放: 不占着环锁调用 allocator */
}

/* 把 g_stop(信号 handler 只置它)在锁内同步成 sender 的停止条件 */
static void pkb_set_stop(void)
{
	pthread_mutex_lock(&g_pkb_lock);
	g_pkb_stop_local = 1;
	pthread_cond_broadcast(&g_pkb_cond);
	pthread_mutex_unlock(&g_pkb_lock);
}

/* 发送线程: 取包 -> 有客户端就推 RTP -> free。阻塞只发生在 rtsp_send_packet。 */
static void *sender_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct pktbuf p;

		pthread_mutex_lock(&g_pkb_lock);
		while (g_pkb_cnt == 0 && !g_pkb_stop_local)
			pthread_cond_wait(&g_pkb_cond, &g_pkb_lock);
		if (g_pkb_cnt == 0) {	/* 只可能是 stop_local 且环已空 */
			pthread_mutex_unlock(&g_pkb_lock);
			break;
		}
		p = g_pkb[g_pkb_head];
		g_pkb[g_pkb_head].data = NULL;
		g_pkb_head = (g_pkb_head + 1) % g_pkb_depth;
		g_pkb_cnt--;
		g_pkb_out++;
		pthread_mutex_unlock(&g_pkb_lock);

		if (g_rtsp_port > 0)
			rtsp_send_packet(p.data, p.len, p.key, p.ts90);
		free(p.data);
	}
	return NULL;
}

static void rtsp_ring_report(void)
{
	unsigned long in, drop;

	pthread_mutex_lock(&g_pkb_lock);
	in = g_pkb_in;
	drop = g_pkb_drop;
	pthread_mutex_unlock(&g_pkb_lock);
	printf("rtsp-ring: in=%lu drop=%lu (深度上限 %d)\n",
	       in, drop, g_pkb_depth);
	fflush(stdout);		/* 后台重定向到日志时也要实时可见 */
}

static int pkb_start(int depth)
{
	if (depth < 1)
		depth = 1;
	if (depth > PKB_MAX)
		depth = PKB_MAX;
	g_pkb_depth = depth;
	if (pthread_create(&g_pkb_tid, NULL, sender_thread, NULL) != 0) {
		fprintf(stderr, ">>> rtsp 发送线程创建失败\n");
		return -1;
	}
	g_pkb_started = 1;
	return 0;
}

/* 退出: 先在锁内同步停止条件, 再唤醒并 join, 最后兜底释放残留 */
static void pkb_stop(void)
{
	int i;

	if (!g_pkb_started)
		return;
	pkb_set_stop();
	pthread_join(g_pkb_tid, NULL);
	g_pkb_started = 0;

	pthread_mutex_lock(&g_pkb_lock);
	for (i = 0; i < g_pkb_cnt; i++) {
		int idx = (g_pkb_head + i) % g_pkb_depth;

		free(g_pkb[idx].data);
		g_pkb[idx].data = NULL;
	}
	g_pkb_cnt = 0;
	g_pkb_head = 0;
	pthread_mutex_unlock(&g_pkb_lock);
}

/* P6: fps 在 dump 内部按"文件内帧数 / (末包 ts - 首包 ts)"计算 ——
 * 三个调用点不再各算一遍(旧代码用 trig_ts + post 当分母, 触发晚时虚低)。 */
static int dump(const char *out, unsigned trig_seq, double trig_ts, int pre_i,
		int end_i, unsigned w, unsigned h)
{
	char path[512], jpath[512];
	FILE *f, *j;
	int i, n = 0;
	int nf = 0;		/* 源帧数(按 seq 去重; 一帧多包时 n > nf) */
	int trig_idx = -1;	/* 触发帧在文件内的 0 基帧序号 */
	int have_prev = 0, last_i;
	unsigned prev_seq = 0;
	double first_ts, span, fps;

	snprintf(path, sizeof(path), "%s.h264", out);
	snprintf(jpath, sizeof(jpath), "%s.json", out);
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, ">>> fopen %s 失败: %s (errno=%d)\n",
			path, strerror(errno), errno);
		return -1;
	}
	/* 防御: EACH_IDR 下每个 IDR 都自带 SPS/PPS, 正常不会进这里; 若流首
	 * IDR 仍缺参数集(旧固件/异常), 用已缓存参数集在文件最前面补一次。
	 * 只检查首个包且只补一次, 多 IDR 文件不会重复。 */
	if (pre_i >= 0 && pre_i <= end_i && pre_i < ring_cnt &&
	    ring_at(pre_i)->key &&
	    !rtsp_has_nal(ring_at(pre_i)->data, ring_at(pre_i)->len, 7)) {
		uint8_t sps[sizeof(g_sps)], pps[sizeof(g_pps)];
		static const uint8_t sc[4] = { 0, 0, 0, 1 };
		int sl, pl;

		pthread_mutex_lock(&g_nal_lock);
		sl = g_sps_len;
		pl = g_pps_len;
		if (sl > 0)
			memcpy(sps, g_sps, (size_t)sl);
		if (pl > 0)
			memcpy(pps, g_pps, (size_t)pl);
		pthread_mutex_unlock(&g_nal_lock);
		if (sl > 0 && pl > 0) {
			fwrite(sc, 1, sizeof(sc), f);
			fwrite(sps, 1, (size_t)sl, f);
			fwrite(sc, 1, sizeof(sc), f);
			fwrite(pps, 1, (size_t)pl, f);
		}
	}
	for (i = pre_i; i <= end_i && i < ring_cnt; i++) {
		struct rec *r = ring_at(i);

		if (fwrite(r->data, 1, (size_t)r->len, f) != (size_t)r->len) {
			fprintf(stderr, ">>> 写 %s 失败(seq=%u len=%d): %s (errno=%d)\n",
				path, r->seq, r->len, strerror(errno), errno);
			fclose(f);
			return -1;
		}
		/* 帧数按源帧去重(同一帧的多个包 seq 相同) */
		if (!have_prev || r->seq != prev_seq) {
			have_prev = 1;
			prev_seq = r->seq;
			nf++;
		}
		/* 触发帧(首个包)在文件内的帧序号: 0 基, 与 list[] 的帧顺序一致 */
		if (trig_idx < 0 && r->seq == trig_seq)
			trig_idx = nf - 1;
		n++;
	}
	if (fclose(f) != 0) {
		fprintf(stderr, ">>> fclose %s 失败: %s (errno=%d)\n",
			path, strerror(errno), errno);
		return -1;
	}

	j = fopen(jpath, "w");
	if (!j) {
		fprintf(stderr, ">>> fopen %s 失败: %s (errno=%d)\n",
			jpath, strerror(errno), errno);
		return -1;
	}
	/* fps 分母 = 文件内实际时间跨度(末包 ts - 首包 ts); 单帧文件 span=0,
	 * 兜底 1 ms 防 0 除, 此时 fps 无物理意义但不会写出 inf。 */
	last_i = (end_i < ring_cnt) ? end_i : ring_cnt - 1;
	first_ts = ring_at(pre_i)->ts_ms;
	span = ring_at(last_i)->ts_ms - first_ts;
	if (span < 1)
		span = 1;
	fps = (double)nf * 1000.0 / span;
	fprintf(j, "{\"file\":\"%s.h264\",\"trigger_seq\":%u,"
		"\"trigger_frame_index\":%d,\"trigger_ts_ms\":%.1f,"
		"\"first_idr_seq\":%u,\"first_idr_ts_ms\":%.1f,"
		"\"frames\":%d,\"frame_count\":%d,\"span_ms\":%.1f,"
		"\"width\":%u,\"height\":%u,\"fps\":%.2f,\"list\":[",
		out, trig_seq, trig_idx, trig_ts, ring_at(pre_i)->seq,
		ring_at(pre_i)->ts_ms, n, nf, span, w, h, fps);
	for (i = pre_i; i <= end_i && i < ring_cnt; i++) {
		struct rec *r = ring_at(i);

		/* list 是"每包一条"; seq 与 frame_seq 都是源帧序号(多包时重复),
		 * pkt 是该帧内第几个包。保留 seq 是为了兼容既有解析脚本。 */
		fprintf(j, "%s{\"seq\":%u,\"frame_seq\":%u,\"pkt\":%u,\"ts_ms\":%.1f,"
			"\"cap_ms\":%.1f,\"dq_lag_ms\":%.1f,"
			"\"abs_ms\":%.1f,\"bytes\":%d,\"key\":%d}",
			(i == pre_i) ? "" : ",", r->seq, r->seq, r->pkt, r->ts_ms,
			r->cap_ms, r->ts_ms - r->cap_ms,
			r->abs_ms, r->len, r->key);
	}
	fprintf(j, "]}\n");
	if (ferror(j)) {
		fprintf(stderr, ">>> 写 %s 失败: %s (errno=%d)\n",
			jpath, strerror(errno), errno);
		fclose(j);
		return -1;
	}
	if (fclose(j) != 0) {
		fprintf(stderr, ">>> fclose %s 失败: %s (errno=%d)\n",
			jpath, strerror(errno), errno);
		return -1;
	}
	printf(">>> NPU det_total=%u det_max=%u score_max=%.3f\n",
	       g_det_total, g_det_max, (double)g_score_max);
	if (g_rga_n)
		printf(">>> RGA 缩放均值 Y=%.1f (%ux%u, 采样 %lu)\n",
		       (double)g_rga_sum / g_rga_n, g_rga_w, g_rga_h, g_rga_n);
	if (nf == n)
		printf(">>> 写出 %s.h264 (%d 帧, 触发帧 #%d) + %s.json\n",
		       out, n, trig_idx, out);
	else
		printf(">>> 写出 %s.h264 (%d 包/%d 帧, 触发帧 #%d) + %s.json\n",
		       out, n, nf, trig_idx, out);
	return 0;
}

/* ---------------- 阶段耗时(P9 观察项的证据) ----------------
 * 只统计 --detect 的帧, 三段严格串行在同一采集线程内:
 *   RGA 缩放 -> RKNN 推理 -> 编码(拷入编码缓冲 + OSD + 送帧到取包)
 * 三者均值之和直接解释"检测开启时 ~10 fps、纯录像 ~15 fps"。 */
static double g_t_rga, g_t_npu, g_t_enc;	/* 累计 ms */
static unsigned long g_t_frames;		/* 累计帧数 */
static double g_t_next_ms = 5000.0;		/* 下次打印时刻(相对 t_start) */

static void stage_report(void)
{
	double tot;

	if (!g_t_frames)
		return;
	tot = g_t_rga + g_t_npu + g_t_enc;
	printf(">>> 阶段耗时(均值, %lu 帧): RGA %.2f + RKNN %.2f + 编码 %.2f "
	       "= %.2f ms/帧 -> %.1f fps\n",
	       g_t_frames, g_t_rga / g_t_frames, g_t_npu / g_t_frames,
	       g_t_enc / g_t_frames, tot / g_t_frames,
	       tot > 0 ? 1000.0 * (double)g_t_frames / tot : 0.0);
	fflush(stdout);
}

/* --fault-sim dqbuf:N[,N...]: 解析注入点(仅测试用) */
static int parse_fault_sim(const char *s)
{
	const char *p;
	int maxn = (int)(sizeof(g_fault_dqbuf) / sizeof(g_fault_dqbuf[0]));

	if (strncmp(s, "dqbuf:", 6) != 0)
		return -1;
	p = s + 6;
	while (*p) {
		char *end = NULL;
		long n;

		if (g_fault_cnt >= maxn)
			return -1;
		n = strtol(p, &end, 10);
		if (end == p || n <= 0 || n > 100000000L)
			return -1;
		g_fault_dqbuf[g_fault_cnt++] = (int)n;
		p = end;
		if (*p == ',')
			p++;
		else if (*p)
			return -1;
	}
	return g_fault_cnt > 0 ? 0 : -1;
}

/* 第 N 次 DQBUF 强制返回 -1(模拟链路断开), 每个注入点只生效一次 */
static int fault_sim_dqbuf(void)
{
	int i;

	g_dqbuf_calls++;
	for (i = 0; i < g_fault_cnt; i++)
		if (!g_fault_done[i] && g_fault_dqbuf[i] == (int)g_dqbuf_calls) {
			g_fault_done[i] = 1;
			fprintf(stderr, ">>> [fault-sim] 第 %u 次 DQBUF 注入失败\n",
				g_dqbuf_calls);
			fflush(stderr);
			errno = EIO;
			return -1;
		}
	return 0;
}

static void usage(void)
{
	printf(
"用法: ./evrec -d <video> -w <宽> -h <高> -o <输出前缀> [选项]\n"
"\n"
"必填/常用:\n"
"  -d DEV            采集设备, 如 /dev/video5\n"
"  -w W              宽, 如 1920\n"
"  -h H              高, 如 1080 (单独写 -h 则打印本帮助)\n"
"  -o PREFIX         输出前缀, 生成 PREFIX-<时间>-<序号>.h264/.json\n"
"  --pre SEC         事件前保留秒数 (默认 2)\n"
"  --post SEC        事件后继续录秒数 (默认 1)\n"
"  --ring-mb MB      环形缓冲上限(默认 24)\n"
"  --kbps KBPS       编码码率 (默认 4000)\n"
"\n"
"触发:\n"
"  --trigger FILE    监听文件 mtime 变化触发; 不给则等 SIGUSR1\n"
"  --max-events N    最多录 N 个事件(0=不限, 默认 0)\n"
"\n"
"检测(内置 YOLOv8n):\n"
"  --detect          启用检测, 命中即自动触发\n"
"  --model PATH      RKNN 模型路径\n"
"  --det-score F     命中分数阈值 (默认 0.45)\n"
"  --det-frames N    连续命中帧数才触发 (默认 3)\n"
"  --obj-thresh F    objectness 阈值 (默认 0)\n"
"  --rga-size WxH    RGA 缩放目标 (默认 640x360)\n"
"\n"
"OSD(叠加源帧序号与时间戳):\n"
"  --osd / --no-osd  开/关, 默认关(不开参数时行为与旧版一致)\n"
"\n"
"RTSP 推流:\n"
"  --rtsp [PORT]     开推流, 省略端口=8554, 0=关闭(默认关闭)\n"
"  --rtsp-ring N     推流投递环深度(默认 8)\n"
"  --rtp-trace FILE  每个 RTP 包一行(seq/ts/mark/nal/fu/s/e/payload/send),\n"
"                    仅 --rtsp 时生效; 默认关闭, 不影响正常发送路径\n"
"\n"
"链路恢复与退出:\n"
"  --recover N       链路故障后重开设备次数 (默认 2, 0=不恢复)\n"
"  --fault-sim dqbuf:N  第 N 次 DQBUF 强制失败(仅测试, 可逗号列多个)\n"
"\n"
"运行时指标: 每秒一行 `rtsp: clients= sent= drop= disc= fds=`;\n"
"            sent=发成的帧, drop=网络侧整包丢弃, disc=对端主动断开(不计入 drop)\n");
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video5", *out = "/root/ev/ev";
	int w = 1920, h = 1080, pre_sec = 3, post_sec = 2, kbps = 8000, gop;
	int i, k, trigger_i = -1, pre_i = -1, end_i = -1;
	int rc = 0;		/* 进程退出码: 0=正常, 1=真错误, 2=链路恢复失败 */
	int reached_max = 0;	/* 因 --max-events 正常结束 */
	int got_term;		/* 主循环退出时是否已收到 SIGTERM */
	int recover_used = 0;	/* 本轮故障链已用掉的恢复次数 */
	int unstable = -1;	/* >=0: 恢复后已采到的帧数(-1=已判定稳定) */
	unsigned seq = 0;	/* 源帧序号: 只有 DQBUF 成功才 +1, 编码包循环不碰 */
	unsigned frame_seq = 0;	/* 当前帧的源帧序号(编码包循环内只读) */
	unsigned trig_seq = 0;	/* P6: 触发帧的源帧序号(写进 json 的 trigger_seq) */
	double trig_ts = 0, t_start;
	struct stat st;
	time_t trig_mtime = 0;

	/* 带值参数缺值时 argv[++i] 取到 argv[argc](NULL), atoi/sscanf(NULL)
	 * 直接段错误 —— 与 "-h 单独出现"是同一个坑, 其余带值参数在这里统一
	 * 预检一遍; 缺值报错退出 1, 不再崩在解析里。 */
	{
		static const char * const need_val[] = {
			"-d", "-w", "--pre", "--post", "--ring-mb", "-o",
			"--trigger", "--kbps", "--model", "--max-events",
			"--det-score", "--det-frames", "--obj-thresh",
			"--rga-size", "--rtsp-ring", "--recover", "--fault-sim",
			"--rtp-trace",
		};
		size_t j;

		for (i = 1; i < argc; i++)
			for (j = 0; j < sizeof(need_val) / sizeof(need_val[0]); j++)
				if (!strcmp(argv[i], need_val[j])) {
					if (i + 1 >= argc) {
						fprintf(stderr, "%s 缺少参数值\n", argv[i]);
						return 1;
					}
					i++;	/* 跳过参数值, 吃法与主循环一致 */
					break;
				}
	}

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d")) dev = argv[++i];
		else if (!strcmp(argv[i], "-w")) w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-?")) {
			usage();
			return 0;
		}
		else if (!strcmp(argv[i], "-h")) {
			/* 单独一个 -h 当"帮助"用: 以前直接 atoi(NULL) 段错误。
			 * 后面跟着数字仍然按"高度"解释, 保持老脚本兼容。 */
			if (i + 1 >= argc) {
				usage();
				return 0;
			}
			h = atoi(argv[++i]);
		}
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
		else if (!strcmp(argv[i], "--osd")) g_osd = 1;
		else if (!strcmp(argv[i], "--no-osd")) g_osd = 0;
		else if (!strcmp(argv[i], "--rtsp")) {
			/* 省略 PORT 时用默认端口 8554; 0 显式关闭 */
			if (i + 1 < argc && argv[i + 1][0] != '-')
				g_rtsp_port = atoi(argv[++i]);
			else
				g_rtsp_port = 8554;
		}
		else if (!strcmp(argv[i], "--rtsp-ring")) g_pkb_depth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rtp-trace")) g_rtp_trace_path = argv[++i];
		else if (!strcmp(argv[i], "--recover")) {
			g_recover = atoi(argv[++i]);
			if (g_recover < 0)
				g_recover = 0;
			if (g_recover > 100)
				g_recover = 100;
		}
		else if (!strcmp(argv[i], "--fault-sim")) {
			if (parse_fault_sim(argv[++i]) < 0) {
				fprintf(stderr, "--fault-sim 只支持 dqbuf:N[,N...] (N>0)\n");
				return 1;
			}
		}
	}
	if (g_rtsp_port < 0 || g_rtsp_port > 65535) {
		fprintf(stderr, "--rtsp 取值 0..65535 (0=关闭)\n");
		return 1;
	}
	/* --rtp-trace 只对 --rtsp 的数据面有意义: 没开推流就提示并忽略,
	 * 不建空文件, 也不让录像路径多一次判空之外的任何开销 */
	if (g_rtp_trace_path) {
		if (g_rtsp_port <= 0) {
			fprintf(stderr, "--rtp-trace 只与 --rtsp 一起生效, 已忽略\n");
			g_rtp_trace_path = NULL;
		} else {
			g_rtp_trace = fopen(g_rtp_trace_path, "w");
			if (!g_rtp_trace) {
				fprintf(stderr, "--rtp-trace 打开 %s 失败: %s\n",
					g_rtp_trace_path, strerror(errno));
				return 1;
			}
			/* 全缓冲 1MB, 每 256 包刷一次: 每包只有一次 snprintf 的内存
			 * 拷贝, 不产生 write 系统调用。P10 实测: 行缓冲(每包一次
			 * write)会把发送节奏拖慢到队列永远不满, 丢包/宏块错误一起
			 * 消失 —— 那是测量改变了被测对象, 不能用。 */
			setvbuf(g_rtp_trace, g_rtp_trace_buf, _IOFBF,
				sizeof(g_rtp_trace_buf));
			atexit(rtp_trace_close);
			printf(">>> RTP trace -> %s\n", g_rtp_trace_path);
		}
	}
	gop = (pre_sec + post_sec) * 15 / 2;
	if (gop < 8)
		gop = 8;

	signal(SIGUSR1, on_sig);
	{	/* SIGTERM 用不带 SA_RESTART 的 sigaction:
		 * 否则阻塞的 DQBUF 会被内核自动重启, 收不到 EINTR, 无法及时退出 */
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_term;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGTERM, &sa, NULL);
	}
	t0_ms = now_ms(CLOCK_MONOTONIC);
	t_start = t0_ms;

	if (cap_open(dev, w, h, 4) < 0)
		return 1;
	/* 分辨率以 S_FMT 回填为准(驱动可能改), stride 同理 */
	w = (int)cap.w;
	h = (int)cap.h;
	if (enc_init(w, h, (int)cap.stride, kbps, gop) < 0)
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
	if (g_osd)
		printf(">>> OSD 开启: #<seq> + t mm:ss.mmm @左上角(4px, sc=2), stride=%u\n",
		       cap.stride);

	/* 采集/编码初始化都成功后再起线程: 失败路径直接 return, 不需要 join */
	if (pkb_start(g_pkb_depth) < 0)
		return 1;
	/* RTSP 在编码头(SPS/PPS)就绪后启动; 启动失败只关推流, 录像照旧 */
	if (g_rtsp_port > 0) {
		signal(SIGPIPE, SIG_IGN);
		if (rtsp_start(g_rtsp_port) < 0) {
			fprintf(stderr, ">>> RTSP 启动失败(端口 %d 被占?), 继续纯录像\n",
				g_rtsp_port);
			g_rtsp_port = 0;
		}
	}

	for (;;) {
		struct v4l2_buffer b;
		struct v4l2_plane pl;
		MppFrame frame = NULL;
		MppPacket pkt = NULL;
		double ts, cap_ms, t_stage = 0, t_rga = 0, t_npu = 0;

		if (g_stop)
			break;
		memset(&pl, 0, sizeof(pl));
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.length = 1;
		b.m.planes = &pl;
		if (fault_sim_dqbuf() < 0 ||
		    xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0) {
			if (g_stop)
				break;	/* SIGTERM: 不再重试, 尽快退出 */
			if (errno == EINTR)
				continue;
			perror("DQBUF");
			goto link_fault;
		}
		ts = now_ms(CLOCK_MONOTONIC);
		{
			/* vb2 采集时间戳: 帧在驱动里完成采集的时刻, 比 DQBUF
			 * 时刻更接近真实曝光完成点。MONOTONIC flag 下它与
			 * CLOCK_MONOTONIC 同零点, cap_ms 与 ts_ms 可直接相减;
			 * 驱动没给(时间戳为 0/非 MONOTONIC)则退化为 DQBUF 时刻,
			 * 口径由首帧打印的 flags 与 json 的 cap_ms 诚实标明。 */
			double vb2_ms = (double)b.timestamp.tv_sec * 1000.0 +
					(double)b.timestamp.tv_usec / 1000.0;
			int flags = (int)(b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK);

			if (g_ts_flags < 0) {
				g_ts_flags = flags;
				if (flags == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
					printf(">>> vb2 ts = MONOTONIC(采集完成时刻, 与 DQBUF 同钟源)\n");
				else
					printf(">>> vb2 ts flags=0x%x(非 MONOTONIC, cap 退化为 DQBUF 时刻)\n",
					       flags);
				fflush(stdout);
			}
			if (vb2_ms <= 0 || flags != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
				cap_ms = ts;
			else
				cap_ms = vb2_ms - t0_ms;
		}
		/* 恢复后连采 30 帧才算链路稳定, 复位"同一故障链"的尝试计数 */
		if (unstable >= 0 && ++unstable >= RECOVER_STABLE_FRAMES) {
			unstable = -1;
			recover_used = 0;
			printf(">>> 链路稳定 %d 帧, 恢复计数复位\n",
			       RECOVER_STABLE_FRAMES);
			fflush(stdout);
		}
		/* 帧序号在这里 +1: 一次 DQBUF = 一个源帧。编码器一帧可能吐出
		 * 多个包(SPS/PPS 单独成包等), 但包循环里只读 frame_seq, 所以
		 * OSD 的 # 与 .json 的 seq 永远按源帧递增, 不会被包数带偏。 */
		frame_seq = seq++;
		/* 阶段计时: t_stage 从"送 RGA"起算, 编码段在取包循环结束后补上 */
		t_stage = now_ms(CLOCK_MONOTONIC);
#ifdef HAVE_RGA
#ifdef HAVE_RKNN
		if (g_detect) {
			rga_scale(cap.map[b.index], w, h);
			t_rga = now_ms(CLOCK_MONOTONIC) - t_stage;
			t_stage += t_rga;
			npu_run(g_rgb);
			t_npu = now_ms(CLOCK_MONOTONIC) - t_stage;
			t_stage += t_npu;
		}
#else
		if (g_detect) {
			rga_scale(cap.map[b.index], w, h);
			t_rga = now_ms(CLOCK_MONOTONIC) - t_stage;
			t_stage += t_rga;
		}
#endif
#endif
		memcpy(mpp_buffer_get_ptr(g_fbuf), cap.map[b.index],
		       (size_t)cap.stride * h * 3 / 2);
		/* OSD 画在编码输入缓冲(最终被编码的那块内存)上, 必须在 encode_put_frame 之前 */
		if (g_osd) {
			g_osd_seq = frame_seq;	/* 与 .json 的 seq/frame_seq 同源 */
			/* 时间标签用 vb2 采集时刻(口径统一), 首帧 ≈ 0 */
			g_osd_ms = cap_ms;
			draw_osd(mpp_buffer_get_ptr(g_fbuf), (int)cap.stride, w, h);
		}
		xioctl(cap.fd, VIDIOC_QBUF, &b);

		mpp_frame_init(&frame);
		mpp_frame_set_width(frame, w);
		mpp_frame_set_height(frame, h);
		mpp_frame_set_hor_stride(frame, (int)cap.stride);
		mpp_frame_set_ver_stride(frame, h);
		mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
		mpp_frame_set_buffer(frame, g_fbuf);
		if (g_mpi->encode_put_frame(g_ctx, frame) != MPP_OK) {
			mpp_frame_deinit(&frame);
			fprintf(stderr, ">>> encode_put_frame 失败\n");
			goto link_fault;
		}
		mpp_frame_deinit(&frame);

		{
			unsigned pkn = 0;	/* 帧内包序号, 只用于 json 的 pkt */
			int enc_fault = 0;

			for (;;) {
				MPP_RET er = g_mpi->encode_get_packet(g_ctx, &pkt);
				void *pos;
				size_t len;
				unsigned char *copy;
				int isk;

				if (er != MPP_OK) {
					fprintf(stderr,
						">>> encode_get_packet 失败 ret=%d\n",
						(int)er);
					enc_fault = 1;
					break;
				}
				if (!pkt)
					break;
				pos = mpp_packet_get_pos(pkt);
				len = mpp_packet_get_length(pkt);
				if (len == 0) {
					mpp_packet_deinit(&pkt);
					continue;
				}
				copy = malloc(len);
				memcpy(copy, pos, len);
				isk = is_key(copy, (int)len);
				/* 首个 IDR 到达即缓存参数集: DESCRIBE 的有界等待和
				 * dump 兜底都依赖它, 不要求开 --rtsp */
				if (isk) {
					pthread_mutex_lock(&g_nal_lock);
					if (g_sps_len == 0 || g_pps_len == 0)
						rtsp_scan_nals(copy, (int)len);
					pthread_mutex_unlock(&g_nal_lock);
				}
				/* seq 只读: 同一帧的所有包共用 frame_seq */
				ring_push(copy, (int)len, frame_seq, pkn, isk,
					  cap_ms);
				/* 同一包的副本进投递环; malloc/memcpy 后立即返回,
				 * 与录像环完全独立, 环满由 pkb_push 丢最旧 */
				pkb_push(copy, (int)len, frame_seq, isk,
					 ts - t_start);
				pkn++;
				mpp_packet_deinit(&pkt);
			}
			if (enc_fault)
				goto link_fault;
		}

		/* 阶段耗时累计(仅 --detect): 编码段 = 拷入缓冲 + OSD + 送帧到取包 */
		if (g_detect) {
			g_t_rga += t_rga;
			g_t_npu += t_npu;
			g_t_enc += now_ms(CLOCK_MONOTONIC) - t_stage;
			g_t_frames++;
			if (ts - t_start >= g_t_next_ms) {
				stage_report();
				g_t_next_ms += 5000.0;
			}
		}

		/* --rtsp 时每 5 秒报一次投递环收支; 未开推流不打印(P4) */
		if (g_rtsp_port > 0)
			while (ts - t_start >= g_pkb_next_ms) {
				rtsp_ring_report();
				g_pkb_next_ms += 5000.0;
			}
		/* --rtsp 开启时每秒一行聚合日志(不是每包一行) */
		if (g_rtsp_port > 0)
			while (ts - t_start >= g_rtsp_next_ms) {
				rtsp_stats_report();
				g_rtsp_next_ms += 1000.0;
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
			/* P6: 触发帧 = 本次循环 DQBUF 到的那一帧(检测/触发文件/信号
			 * 都在这一帧的循环里被看到); 旧代码 dump 时传的是末帧
			 * frame_seq, 导致 json 的 trigger_seq 指向文件最后一帧。
			 * trig_ts 也取该帧的环形时间戳(而不是 DQBUF 时刻的 ts):
			 * 环形 ts 是"编码送帧"时刻, 与 json list 的 ts_ms 同源,
			 * 这样 trigger_ts_ms 恒等于 list 中 trigger_seq 那条的
			 * ts_ms, 前后窗口计算也不会错位。 */
			trig_seq = frame_seq;
			trig_ts = ring_cnt > 0 ? ring_at(ring_cnt - 1)->ts_ms
					       : ts - t_start;
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
			       trig_seq, trig_ts, pre_i, g_pre_seq,
			       ring_cnt, ring_head, g_pushed);
		}
		/* S2: 事件开流式写入(pre-roll 从 pre_i 到 ring 尾;
		 * 之后新包由 ring_push → evw_write 按 seq 过滤追加) */
		if (evw_open(pre_i) != 0) {
			fprintf(stderr, ">>> 事件写入器打开失败, 事件放弃\n");
			g_trigger = 0;
			trigger_i = pre_i = end_i = -1;
			g_pre_seq = 0;
		}
		/* S1(docs/37 §3.1): 事件结束 = 目标离开(最近命中 + post 窗口),
		 * 不再锚定首次触发时刻;检测漏帧不会把持续目标拆成碎事件。
		 * close_reason 四值(target_left/max_duration/disk_low/sigterm)
		 * 在 S3/S4 引入,本步先用 target_left 语义。 */
		if (trigger_i >= 0 &&
		    g_last_detect_ms > 0 &&
		    now_ms(CLOCK_MONOTONIC) - g_last_detect_ms > post_sec * 1000.0) {
			int ok;

			end_i = ring_cnt - 1;
			/* S2: 事件已在流式写入, 这里只收尾(flush+close+rename+JSON) */
			ok = evw_close("target_left", trig_seq, trig_ts) == 0 && !g_ev_werr;
			if (!ok) {
				fprintf(stderr, ">>> 事件落盘失败\n");
				rc = 1;
				break;
			}
			printf(">>> 事件 %d 完成, 继续等待下一个事件\n", g_events);
			/* S1(docs/37 §3.4): 不再 ring_clear —— ring 照滚,
			 * 下一个事件的 pre-roll 从这里取; 复位集合只收窄到
			 * 触发/事件状态。g_streak/g_det_announced 不动:
			 * 目标仍在场时 streak 已在增长, 清零会让下一个触发
			 * 重新等门限, 反而漏事件。 */
			g_trigger = 0;
			g_pre_seq = 0;
			trigger_i = pre_i = end_i = -1;
			if (g_max_events && g_events >= g_max_events) {
				reached_max = 1;
				break;
			}
		}
		continue;

link_fault:
		/* 采集链路故障: 只重建 V4L2, MPP 编码器与 RTSP 服务端不动 */
		{
			int used;

			printf(">>> 链路故障, 开始恢复(上限 %d 次)\n", g_recover);
			fflush(stdout);
			/* 还没恢复成功就又坏 -> 沿用本轮剩余次数, 防无限重开 */
			used = link_recover(dev, w, h,
					    unstable >= 0 ? recover_used : 0);
			if (used < 0) {
				if (g_stop) {
					printf(">>> 恢复中止(SIGTERM)\n");
					fflush(stdout);
					break;	/* 走优雅退出, 不算链路失败 */
				}
				printf(">>> 恢复失败, 退出\n");
				fflush(stdout);
				rc = 2;
				break;
			}
			recover_used = used;
			unstable = 0;
			printf(">>> 恢复成功, 继续采集(frame_seq 从 %u 起)\n",
			       frame_seq);
			fflush(stdout);
		}
	}

	/* P9: 检测开启时收尾打一行三阶段均值(每 5 s 也打过), 作为
	 * "RGA+RKNN+编码三者串行 -> ~10 fps" 的可复核数字 */
	stage_report();
	/* 先记下退出原因: 停线程会唤醒发送线程, 不能事后区分 */
	got_term = g_stop;
	/* SIGTERM 且正在录制: 先把当前事件落盘(环还完整), 再停 RTSP/线程/采集。
	 * 主循环已退出, 环不再变化, dump 只读环与全局状态, 无并发风险。 */
	if (got_term && trigger_i >= 0 && !rc && ring_cnt > 0) {
		int pi = pre_i;

		if (g_pre_seq)
			for (i = 0; i < ring_cnt; i++)
				if (ring_at(i)->seq == g_pre_seq) {
					pi = i;
					break;
				}
		if (pi < 0)
			pi = ring_first_idr_from(0);
		if (pi >= 0) {
			char outn[512];

			/* 事件没走完就被 TERM: dump 内部按环内实际时间跨度算 fps */
			snprintf(outn, sizeof(outn), "%s-%02d", out, g_events);
			if (dump(outn, trig_seq, trig_ts, pi, ring_cnt - 1, w, h) == 0) {
				printf(">>> SIGTERM: 进行中的事件已落盘 %s\n", outn);
				fflush(stdout);
			} else {
				fprintf(stderr, ">>> SIGTERM: 事件落盘失败\n");
			}
		}
	}
	/* 信号 handler 只置 g_stop: 这里在锁内把它同步成 sender 的停止条件,
	 * 先让 RTSP 线程退, 再 join sender 并释放投递环残留 */
	pkb_set_stop();
	rtsp_stop();
	pkb_stop();
	if (cap.fd >= 0) {
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		xioctl(cap.fd, VIDIOC_STREAMOFF, &t);
	}
	/* SIGTERM: 优雅退出; 进行中的事件已在上面的 dump 落盘。
	 * 若同时已有真错误(rc!=0), 以错误码为准, 不掩盖失败。 */
	if (got_term && !rc) {
		printf(">>> 收到 SIGTERM, 已停止\n");
		return 0;
	}
	/* 达到 --max-events 属正常结束(事件已在循环内落盘); 落盘失败已打印过 */
	if (reached_max || rc)
		return rc;
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
	if (dump(out, trig_seq, trig_ts, pre_i, end_i, w, h) < 0) {
		fprintf(stderr, ">>> 事件落盘失败\n");
		return 1;
	}
	return 0;
}
