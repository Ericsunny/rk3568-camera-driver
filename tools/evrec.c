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
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <dirent.h>
#include "rtsp_hub.h"
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
	rtsp_hub_set_stop();
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
/* docs/37 §5 要求 JSON 自描述: 事件期间被合并的触发数、事件开始前冷却期
 * 被跳过的触发数。计数必须放在检测侧"每次连击 episode"上 —— 放在主循环
 * 触发门会每帧都命中, 按帧放大(命中帧率 12 fps 就是 12 倍)。 */
static unsigned g_ev_coalesced;		/* 本事件期间被合并的触发数 */
static unsigned g_ev_cdskip_pend;	/* 冷却期内累计(等下一个事件认领) */
static unsigned g_ev_cdskip;		/* 本事件开始前冷却期跳过数 */
static int ev_active(void);
static int ev_cooldown_active(void);
/* 观测(docs/37 §8.1): "连击开始/检测触发"只在状态变化时打印, 连续命中
 * 35 秒也只有 2 行 —— 据此无法区分"没有命中"与"一直命中但不打印", T3 的
 * 判读卡在这里。补两类客观计数: 5 秒一行的命中率统计 + 连击结束一行。
 * 纯观测, 不改变任何判定逻辑(命中仍按单帧 score 门限刷新 last_detect_ms)。 */
static unsigned g_det_frm_win, g_det_hit_win;	/* 本统计窗内帧数 / 命中帧数 */
static double g_det_peak_win;			/* 本统计窗内最高分 */
static double g_det_mean_sum;			/* 本统计窗内 NPU 输入均值之和 */
static unsigned g_det_mean_n;			/* 本统计窗内均值采样帧数 */
static double g_det_next_ms = 5000.0;		/* 下次统计打印(相对 t0_ms) */
static double g_det_end_ms;			/* 上次"连击结束"打印时刻 */
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
	{	/* 诊断 + 观测: g_int8 实际内容均值。
		 * 前 3 帧打明细(定位"有没有数据"); 均值同时进 5 s 统计窗口 ——
		 * 这是"检测输入是不是当帧"的唯一可观测判据: 遮住 J10 后均值
		 * 必须骤降; 若不降, 说明 RGA/NPU 读到的是陈旧缓冲, 属代码缺陷
		 * (docs/37 §8.2 S0)。旧实现的 mean 只在前 3 帧打印, 拿它比较
		 * "遮住前后"是无效证据。 */
		static int dbg;
		long s = 0;
		double in_mean;

		for (i = 0; i < sz; i += 64)
			s += (unsigned char)g_int8[i];
		in_mean = sz >= 64 ? (double)s / (double)(sz / 64) : 0.0;
		g_det_mean_sum += in_mean;
		g_det_mean_n++;
		if (dbg++ < 3)
			fprintf(stderr,
				"[npu] in: sz=%u mean=%.1f rgb0=%u/%u/%u int8_0=%d\n",
				(uint32_t)sz, in_mean,
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
					/* 本 episode 落在哪: 冷却期 -> 记跳过;
					 * 事件进行中 -> 记合并(docs/37 §5) */
					if (ev_cooldown_active())
						g_ev_cdskip_pend++;
					else if (ev_active())
						g_ev_coalesced++;
				}
				g_trigger = 1;
			}
		} else {
			/* 观测: 连击结束必须可见 —— T3 的正确判读是"整段
			 * 没有出现任何一行连击结束"(说明 streak 一次没断),
			 * 而不是"没有命中日志"。噪声场景可能逐帧翻转, 节流 1 行/秒。 */
			if (g_streak > 0 &&
			    now_ms(CLOCK_MONOTONIC) - g_det_end_ms > 1000.0) {
				double tnow = now_ms(CLOCK_MONOTONIC);

				g_det_end_ms = tnow;
				printf(">>> 检测连击结束 streak=%d (最近命中 %.1fs 前)\n",
				       g_streak, g_last_detect_ms > 0 ?
				       (tnow - g_last_detect_ms) / 1000.0 : -1.0);
				fflush(stdout);
			}
			g_streak = 0;
			g_det_announced = 0;
		}
		/* 观测: 5 秒一行命中率 —— 唯一能证明/否证"无目标仍在命中"
		 * 的数据(命中 = 有 class0 框且 score >= g_det_score)。 */
		g_det_frm_win++;
		if (hit)
			g_det_hit_win++;
		if (maxscore > g_det_peak_win)
			g_det_peak_win = maxscore;
		{
			double t = now_ms(CLOCK_MONOTONIC);

			if (t - t0_ms >= g_det_next_ms) {
				printf(">>> 检测统计 %.0fs: 命中 %u/%u 帧 (%.0f%%), "
				       "峰值 %.3f, 输入均值 %.1f, streak=%d, "
				       "最近命中 %.1fs 前\n",
				       (t - t0_ms) / 1000.0, g_det_hit_win,
				       g_det_frm_win, g_det_frm_win ?
				       100.0 * g_det_hit_win / g_det_frm_win : 0.0,
				       g_det_peak_win, g_det_mean_n ?
				       g_det_mean_sum / g_det_mean_n : 0.0, g_streak,
				       g_last_detect_ms > 0 ?
				       (t - g_last_detect_ms) / 1000.0 : -1.0);
				fflush(stdout);
				g_det_next_ms += 5000.0;
				g_det_hit_win = 0;
				g_det_frm_win = 0;
				g_det_peak_win = 0;
				g_det_mean_sum = 0;
				g_det_mean_n = 0;
			}
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
static unsigned long g_ev_bytes;	/* 当前事件已写字节 */
static unsigned long g_ev_last_pkts;	/* 已写入的包游标(=g_pushed 快照) */
/* S3: 分段语义 —— event_id 跨段不变; segment_index 每切一段 +1;
 * max-duration 切段不进 cooldown(docs/37 §3.2)。 */
static unsigned g_ev_event_seq;		/* 进程内事件序号: 命名与 event_id 同源 */
static unsigned g_ev_event_id;		/* 当前事件的 event_id(跨段不变) */
static unsigned g_ev_segment;		/* 当前事件内段号(0 基) */
static int g_max_event_sec;
/* S4: cooldown + 磁盘水位(docs/37 §3.2/§3.3) */
static int g_cooldown_sec;		/* --cooldown, 0=关(默认) */
static double g_cooldown_until_ms;	/* 冷却截止(单调 ms); 只在 target_left 后设置 */
static int g_disk_min_mb = 1000;	/* --disk-min-mb; 300s 段实测 62MB,500 仅容 8 段 */
static int g_disk_hyst_mb = 1000;	/* 滞回带: 回升须越过 min+hyst 才恢复 */
static int g_disk_stopped;		/* 1=水位停录中 */
static double g_disk_next_check;	/* 事件中每 10s 查一次 */
static char g_disk_dir[512] = "/root";	/* 水位量测目录: -o 前缀所在目录 */

static int ev_active(void)
{
	return g_ev_fp != NULL;
}

static int ev_cooldown_active(void)
{
	return g_cooldown_until_ms > 0 &&
	       now_ms(CLOCK_MONOTONIC) < g_cooldown_until_ms;
}
static double g_ev_first_ts, g_ev_last_ts;	/* 媒体跨度: 首包/末包 ts_ms */
static const unsigned EV_BUF_CAP = 4u << 20;
static int g_ev_werr;			/* 写失败标志(主循环收尾时清) */
static char g_out_base[512];		/* 事件输出前缀(带运行时间戳, main 里赋值) */

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

static void evw_write(const unsigned char *d, int len, unsigned seq, unsigned pkt, double ts_ms)
{

	/* 包级游标: g_pushed 是全局单调包计数; 事件开启时快照过 pre-roll 尾,
	 * 这里只放行之后的新包(同帧多包计数不同, 不会被误滤) */
	if (g_pushed <= g_ev_last_pkts) {
		fprintf(stderr, "[evw] skip: pushed=%lu last=%lu\n",
			g_pushed, g_ev_last_pkts);
		return;
	}
	g_ev_last_pkts = g_pushed;
	if (g_ev_first_ts == 0)
		g_ev_first_ts = ts_ms;
	g_ev_last_ts = ts_ms;

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
 * 之后 ring_push 的新包按包级游标(g_ev_last_pkts)过滤。 */

static struct rec *ring_at(int i);	/* 前向声明 */

/* S3: ring 内最新的 IDR 下标。续段(pre_i<0)必须从这里起写:
 * 旧实现把 pre_i<0 当成"从 ring 头写", 每段都把整个环(最多
 * --ring-mb)的历史再写一遍 —— 板上实测段体积 2.1/4.0/6.1/8.1MB
 * 逐段递增就是这个原因, 且新段开头是任意历史帧而非 IDR。 */
static int ring_last_idr(void)
{
	int i;

	for (i = ring_cnt - 1; i >= 0; i--)
		if (ring_at(i)->key)
			return i;
	/* 环内无 IDR(切段条件保证不会走到): 宁可只写尾包, 也不把整个环
	 * 倒进新段 —— 后者正是旧实现"每段体积递增"的来源。 */
	return ring_cnt > 0 ? ring_cnt - 1 : 0;
}

/* S3(docs/37 §3.2): "事件是否仍在场" —— 切段续录与收尾共用同一判据:
 * 最近一次命中/触发落在 post 窗口内才允许续段, 否则事件到此结束。
 * 没有这条门, 切段链会在无目标时自我续命(旧 S3 板上实测: 镜头无人
 * 仍连录 5 段、只有 SIGTERM 能停 —— 无限录 = 磁盘写满)。
 * 手动触发模式没有检测命中刷新 g_last_detect_ms, 触发时刻已写入该值
 * (见主循环), 因此 max-event-sec 到点即收尾, 行为可预期。 */
static int event_alive(int post_sec)
{
	return g_last_detect_ms > 0 &&
	       now_ms(CLOCK_MONOTONIC) - g_last_detect_ms <= post_sec * 1000.0;
}

/* S4: 磁盘可用 MB —— 量"实际写入的那个文件系统"(-o 前缀所在目录)。
 * 写死 /root 会在 -o 指向其它挂载点时量错盘, 水位形同虚设。 */
static long disk_free_mb(void)
{
	struct statvfs v;

	if (statvfs(g_disk_dir, &v) != 0 &&
	    statvfs("/", &v) != 0)
		return -1;	/* 查不到就不拦录像 */
	return (long)((double)v.f_bavail * v.f_frsize / (1024 * 1024));
}

/* S4: 水位决策 —— 返回 1=允许开新事件/继续录, 0=停录(双阈值+滞回)。
 * 每次调用最多 10s 查一次 statvfs, 其余用缓存值。 */
static int disk_allow(void)
{
	static long cached = -1;
	double now = now_ms(CLOCK_MONOTONIC);

	if (now >= g_disk_next_check) {
		cached = disk_free_mb();
		g_disk_next_check = now + 10000.0;
	}
	if (cached < 0)
		return 1;		/* 查询失败不拦录像 */
	if (g_disk_stopped) {
		if (cached >= g_disk_min_mb + g_disk_hyst_mb) {
			g_disk_stopped = 0;
			printf(">>> 磁盘回升(%ldMB >= %dMB @%s), 恢复录像\n",
			       cached, g_disk_min_mb + g_disk_hyst_mb, g_disk_dir);
		}
	} else if (cached < g_disk_min_mb) {
		g_disk_stopped = 1;
		printf(">>> 磁盘低水位(%ldMB < %dMB @%s), 停止开启新事件\n",
		       cached, g_disk_min_mb, g_disk_dir);
	}
	return !g_disk_stopped;
}

/* 公共: 按 (事件序号, 段号) 开 .part + 写 pre-roll + 置包游标。
 * event_id / segment 由调用方决定, 本函数不改写它们 —— 旧实现每次
 * open 都把 g_ev_segment 归零、重新生成 event_id, 续段因此变成"新事件"。
 * 命名: 段 0 = PREFIX-NN, 续段 = PREFIX-NN-sN; NN 只随事件递增。 */
static int evw_open_at(int pre_i, unsigned serial, unsigned segment,
		       unsigned event_id)
{
	char path[560];
	int i, from = pre_i >= 0 ? pre_i : ring_last_idr();

	/* 评审防御: 事件已在写时禁止重开 —— 重开等于 fopen("wb") 截断
	 * 当前事件 + 泄漏旧 fd(板上实测 168 次/25s 的根因)。 */
	if (g_ev_fp != NULL) {
		fprintf(stderr, "[evw] open refused: event already active\n");
		return -1;
	}

	if (segment == 0)
		snprintf(g_ev_path, sizeof(g_ev_path), "%s-%02u", g_out_base, serial);
	else
		snprintf(g_ev_path, sizeof(g_ev_path), "%s-%02u-s%u", g_out_base,
			 serial, segment);
	snprintf(path, sizeof(path), "%s.part", g_ev_path);
	g_ev_fp = fopen(path, "wb");
	if (!g_ev_fp) {
		fprintf(stderr, ">>> evw_open %s 失败: %s\n",
			path, strerror(errno));
		return -1;
	}
	g_ev_bytes = 0;
	g_ev_werr = 0;
	g_ev_first_ts = g_ev_last_ts = 0;
	g_ev_event_id = event_id;
	g_ev_segment = segment;
	/* pre-roll: from(最近 IDR) 到 ring 尾; 这些包之后由 evw_write 过滤。
	 * 续段时 from 就是切段那个 IDR, 与上一段末尾的同一包重叠 1 个,
	 * 这是刻意的: 两段都从 IDR 起, 各自独立可解码。 */
	for (i = from; g_ev_fp && i < ring_cnt; i++) {
		struct rec *r = ring_at(i);
		if (fwrite(r->data, 1, (size_t)r->len, g_ev_fp) != (size_t)r->len) {
			g_ev_werr = 1;
			break;
		}
		g_ev_bytes += (unsigned)r->len;
	}
	/* 包级游标: pre-roll 写完后全局已推包数为界, 之后的新包才 append */
	g_ev_last_pkts = g_pushed;
	return 0;
}

/* 新事件: 序号 +1, event_id = 序号, 段号从 0 起。
 * 序号只在 open 成功后才提交, 失败不吞号(便于对账文件名连续)。 */
static int evw_open_new(int pre_i)
{
	unsigned serial = g_ev_event_seq + 1;
	int rc = evw_open_at(pre_i, serial, 0, serial);

	if (rc == 0) {
		g_ev_event_seq = serial;
		g_ev_coalesced = 0;		/* 事件级: 新事件从头算 */
		g_ev_cdskip = g_ev_cdskip_pend;	/* 冷却期跳过的归到本事件 */
		g_ev_cdskip_pend = 0;
	}
	return rc;
}

/* 续段: 事件序号与 event_id 不变, 段号 +1(docs/37 §3.2)。 */
static int evw_open_next(int pre_i)
{
	return evw_open_at(pre_i, g_ev_event_seq, g_ev_segment + 1,
			   g_ev_event_id);
}

/* 事件完成: 计数 + 复位事件级状态(段号归零, 下一事件从 0 起)。
 * 只做计数与清理; --max-events 的 break 留给主循环。
 * 注意: 段文件落盘不调用本函数 —— 分段落盘 ≠ 事件完成。 */
static void evw_event_finish(void)
{
	g_events++;
	g_ev_segment = 0;
	g_ev_event_id = 0;
	g_pre_seq = 0;
	g_trigger = 0;
}

/* S4 评审修正: 事件结束的唯一收口 —— 打印 + 计数 + cooldown 置位。
 * 所有结束路径(目标离场 / 水位停录 / 续段失败)都只调它一次。
 * 旧实现里 disk_low 分支先 finish 又掉进 else 再 finish: g_events 双计、
 * 日志打两遍(第二次 event_id 已被清零)、还误报"续段失败"。
 * cooldown 只认 target_left: 切段(水位/时长)不该压制下一个真实目标。 */
static void evw_event_end(const char *reason)
{
	printf(">>> 事件 %u 完成(%u 段, %s)\n", g_ev_event_id,
	       g_ev_segment + 1, reason ? reason : "?");
	evw_event_finish();
	if (g_cooldown_sec > 0 && reason && !strcmp(reason, "target_left"))
		g_cooldown_until_ms = now_ms(CLOCK_MONOTONIC) +
				      (double)g_cooldown_sec * 1000.0;
}

/* S2: 事件收尾 —— flush+close+rename+JSON。close_reason 由调用方给。
 * S3: 本函数只负责"落一段", 不再自增 g_events —— 分段落盘不等于
 * 事件完成, 事件计数由调用方在事件真正结束时 +1(--max-events 语义)。 */
static int evw_close(const char *reason, unsigned trig_seq, double trig_ts)
{
	char path[560], jpath[560];
	FILE *j;

	ev_flush();
	if (g_ev_fp && fclose(g_ev_fp) != 0)
		g_ev_werr = 1;
	g_ev_fp = NULL;
	snprintf(path, sizeof(path), "%s.part", g_ev_path);
	snprintf(jpath, sizeof(jpath), "%s.json", g_ev_path);
	{
		char final[560];
		snprintf(final, sizeof(final), "%s.h264", g_ev_path);
		if (rename(path, final) != 0) {
			fprintf(stderr, ">>> rename %s 失败: %s\n",
				path, strerror(errno));
			return -1;
		}
	}
	j = fopen(jpath, "w");
	if (j) {
		fprintf(j,
			"{\"file\":\"%s.h264\",\"trigger_seq\":%u,"
			"\"trigger_ts_ms\":%.1f,\"close_reason\":\"%s\","
			"\"bytes\":%lu,\"duration_ms\":%.1f,"
			"\"event_id\":%u,\"segment_index\":%u,"
			"\"coalesced\":%u,\"cooldown_skipped\":%u,"
			"\"disk_free_mb\":%ld,"
			"\"first_ts_ms\":%.1f,\"last_ts_ms\":%.1f,"
			"\"write_error\":%s}\n",
			g_ev_path, trig_seq, trig_ts, reason,
			g_ev_bytes, g_ev_last_ts - g_ev_first_ts,
			g_ev_event_id, g_ev_segment,
			g_ev_coalesced, g_ev_cdskip, disk_free_mb(),
			g_ev_first_ts, g_ev_last_ts,
			g_ev_werr ? "true" : "false");
		fclose(j);
	}
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
	 * 包级游标过滤; 写失败只置 g_ev_werr, 由主循环收尾) */
	if (g_ev_fp)
		evw_write(d, len, seq, pkt, r->ts_ms);
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

#define RTSP_REQ_MAX 4096
#define RTSP_SDP_MAX 2048

/* 发送结果: 0=整包已发出, DROP=整包丢弃(客户端还在), FATAL=连接已坏 */
static int g_rtsp_port;			/* 0=关闭, --rtsp PORT 开启 */

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
		rtsp_hub_notify_recover();	/* H1: hub 内部重置发送端状态 */
		return i + 1;
	}
	return -1;
}

/* ---------------- RTSP 服务端+投递环: 已提炼到 tools/rtsp_hub.c(H1) ----------------
 *
 * 本区域代码(原 1400-2520 行, RTSP 信令/RTP 打包/pkb 投递环/accept/发送线程)
 * 原样提炼到 rtsp_hub.c —— H1 单客户端行为等价替换, 多客户端(H3)在该文件演进。
 * 这里只保留桥接:
 *   - hub 的停止注入: evrec 的信号 handler 置 g_stop, 主循环轮询转调
 *     rtsp_hub_set_stop()(避免 signal handler 直接碰 hub 内部锁);
 *   - 对外符号: rtsp_scan_nals / rtsp_has_nal / rtsp_start / rtsp_stop /
 *     pkb_push / pkb_set_stop / rtsp_ring_report / rtsp_stats_report
 *     (签名与原 static 版本一致, 由 rtsp_hub.c 提供)。 */

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
		uint8_t sps[128], pps[128];
		static const uint8_t sc[4] = { 0, 0, 0, 1 };
		int sl = 0, pl = 0;

		rtsp_hub_get_ps(sps, sizeof(sps), pps, sizeof(pps), &sl, &pl);
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
"  --max-event-sec N 单段最长秒数: 到点且 ring 尾为 IDR 时切段,\n"
"                    event_id 跨段不变, segment_index+1; 目标已离场则\n"
"                    直接收尾不再续段(实际段长最多超 1 个 GOP)\n"
"  --max-events N    最多录 N 个事件(0=不限, 默认 0)\n"
"  --cooldown S      事件结束后的冷却秒数(target_left 后, 0=关)\n"
"  --disk-min-mb M   磁盘低于 M MB 停录, 回升 M+1000 才恢复(默认 1000;\n"
"                    量的是 -o 前缀所在文件系统)\n"
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
"RTSP 推流(多客户端):\n"
"  --rtsp [PORT]     开推流, 省略端口=8554, 0=关闭(默认关闭)\n"
"  --rtsp-clients N  最大并发播放客户端数(默认 4, 超出的 PLAY 收 453)\n"
"  --rtsp-deadline-s N  慢客户端硬上限: 连续 N 秒零发送进展判死(默认 5)\n"
"  --rtsp-ring N     推流分发环深度(包数, 默认 30)\n"
"  --rtsp-ring-mb M  推流分发环字节上限(MB, 默认 8; 双上限, 字节优先逐出)\n"
"  --rtp-trace FILE  每个 RTP 包一行(seq/ts/mark/nal/fu/s/e/payload/send),\n"
"                    仅 --rtsp 时生效; 默认关闭, 不影响正常发送路径\n"
"\n"
"链路恢复与退出:\n"
"  --recover N       链路故障后重开设备次数 (默认 2, 0=不恢复)\n"
"  --fault-sim dqbuf:N  第 N 次 DQBUF 强制失败(仅测试, 可逗号列多个)\n"
"\n"
"运行时指标: 每秒 `rtsp: conn= play=N/M sent= epoch= rej453= fds=` 加每个\n"
"            客户端一行 `rtsp cN: sent=包/字节 skip= evict= dead= lag= idle=`;\n"
"            evict=被环逐出(被动丢), skip=等 IDR 主动跳帧, dead=发送判死\n");
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video5", *out = "/root/ev/ev";
	int w = 1920, h = 1080, pre_sec = 3, post_sec = 2, kbps = 8000, gop;
	int max_event_sec = 0;
	int i, k, pre_i = -1, end_i = -1;
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
			"--rga-size", "--rtsp-ring", "--rtsp-ring-mb",
			"--rtsp-clients", "--rtsp-deadline-s", "--recover",
			"--fault-sim", "--rtp-trace",
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
		else if (!strcmp(argv[i], "-o"))
			out = argv[++i];
		else if (!strcmp(argv[i], "--trigger")) g_trig_path = argv[++i];
		else if (!strcmp(argv[i], "--kbps")) kbps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--detect")) g_detect = 1;
		else if (!strcmp(argv[i], "--model")) g_model = argv[++i];
		else if (!strcmp(argv[i], "--max-events")) g_max_events = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--max-event-sec")) g_max_event_sec = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cooldown")) g_cooldown_sec = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--disk-min-mb")) g_disk_min_mb = atoi(argv[++i]);
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
		else if (!strcmp(argv[i], "--rtsp-ring")) rtsp_hub_set_ring_depth(atoi(argv[++i]));
		else if (!strcmp(argv[i], "--rtsp-ring-mb")) rtsp_hub_set_ring_mb(atoi(argv[++i]));
		else if (!strcmp(argv[i], "--rtsp-clients")) rtsp_hub_set_max_clients(atoi(argv[++i]));
		else if (!strcmp(argv[i], "--rtsp-deadline-s")) rtsp_hub_set_deadline(atoi(argv[++i]));
		else if (!strcmp(argv[i], "--rtp-trace")) rtsp_hub_set_trace_path(argv[++i]);
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
		/* 事件前缀必须带运行时间戳: 否则两次运行都写 PREFIX-01.h264,
		 * 第二次 fopen("wb") 直接覆盖第一次的事件(285923c 说明里声称
		 * 做了, 实际漏了 —— 板上 /root/ev/s5-01.part 即证据)。 */
		snprintf(g_out_base, sizeof(g_out_base), "%s", out);
		/* S4: 水位量测目录 = 输出前缀的目录部分(无目录则用 /) */
		{
			const char *slash = strrchr(g_out_base, '/');
			size_t n = slash ? (size_t)(slash - g_out_base) : 0;

			if (n > 0 && n + 1 < sizeof(g_disk_dir)) {
				memcpy(g_disk_dir, g_out_base, n);
				g_disk_dir[n] = '\0';
			} else {
				snprintf(g_disk_dir, sizeof(g_disk_dir), "%s",
					 slash ? "/" : ".");
			}
		}
	}
	printf(">>> 采集 %s %dx%d,pre=%ds post=%ds ring=%zuMB gop=%d\n",
	       dev, w, h, pre_sec, post_sec, ring_limit >> 20, gop);
	if (g_osd)
		printf(">>> OSD 开启: #<seq> + t mm:ss.mmm @左上角(4px, sc=2), stride=%u\n",
		       cap.stride);

	/* 采集/编码初始化都成功后再起线程: 失败路径直接 return, 不需要 join */
	if (pkb_start(0) != 0)
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
				if (isk)
					rtsp_scan_nals(copy, (int)len);
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

		/* S4/H1: 投递环收支与聚合日志移入 hub(自带节拍), 主循环只轮询 */
		rtsp_hub_poll_reports(ts - t_start);

		/* ---------------- 事件状态机(docs/37 §3.1/§3.2) ----------------
		 * 每帧顺序: ①分段 ②触发开新事件 ③目标离场收尾。
		 * 三块共享 g_ev_fp/trig_seq/trig_ts 一组状态, 且都以"事件是否
		 * 在写"为门, 互斥, 不会同帧打架。事件进行中的后续命中只刷
		 * g_last_detect_ms(检测回调内), 不再重进触发块 —— 旧实现每帧
		 * 无条件 evw_open, 无触发时也会开幽灵事件(板上"镜头无人连录
		 * 5 段"就是它), 且会覆盖 trig_seq/trig_ts。 */

		/* ① 分段: 事件在写 + 配额 > 0 + 媒体跨度到点 + ring 尾是 IDR。
		 * 段边界必须落在 IDR 上, 段文件才能独立解码; 因此实际段时长会
		 * 略超 --max-event-sec(最多一个 GOP)。 */
		if (g_ev_fp != NULL && g_max_event_sec > 0 &&
		    g_ev_last_ts - g_ev_first_ts >=
				(double)g_max_event_sec * 1000.0 &&
		    ring_cnt > 0 && ring_at(ring_cnt - 1)->key) {
			int alive = event_alive(post_sec);
			/* 目标仍在场 + 水位允许 -> 切段续录; 否则本次就是事件结束:
			 * 已离场用 target_left, 水位不足用 disk_low
			 * (max_duration 只是同时撞上的上限, 不是结束原因)。 */
			int disk_block = alive && !disk_allow();
			const char *reason = !alive ? "target_left"
			                   : disk_block ? "disk_low"
			                   : "max_duration";
			int keep = alive && !disk_block;

			if (evw_close(reason, trig_seq, trig_ts) != 0 || g_ev_werr) {
				fprintf(stderr, ">>> 分段收尾失败\n");
				rc = 1;
				break;
			}
			if (keep && evw_open_next(-1) != 0) {
				fprintf(stderr, ">>> 续段失败, 事件提前结束\n");
				keep = 0;
			}
			if (!keep) {
				evw_event_end(reason);	/* 唯一出口, 只收口一次 */
				if (g_max_events && g_events >= g_max_events) {
					reached_max = 1;
					break;
				}
				continue;	/* 事件已结束, 本帧不走 ②③ */
			}
			printf(">>> 分段 %u (event_id=%u, IDR 边界)\n",
			       g_ev_segment, g_ev_event_id);
		}

		/* ①b 事件内水位检查: 与是否分段无关 —— 默认 --max-event-sec 0
		 * (不分段)时, 进行中的长事件也必须能被低水位收尾, 否则磁盘照样
		 * 写满, 而防写满正是 S4 的全部意义。disk_allow() 自带 10 s 缓存,
		 * 每帧只是一次比较。 */
		if (g_ev_fp != NULL && !disk_allow()) {
			if (evw_close("disk_low", trig_seq, trig_ts) != 0 ||
			    g_ev_werr) {
				fprintf(stderr, ">>> 事件落盘失败\n");
				rc = 1;
				break;
			}
			evw_event_end("disk_low");
			if (g_max_events && g_events >= g_max_events) {
				reached_max = 1;
				break;
			}
			continue;
		}

		/* ② 触发: 信号 或 触发文件 mtime 变化 */
		if (!g_trigger && g_trig_path && stat(g_trig_path, &st) == 0 &&
		    st.st_mtime != trig_mtime) {
			trig_mtime = st.st_mtime;
			g_trigger = 1;
		}
		if (g_trigger && g_ev_fp == NULL) {
			/* S4: cooldown 期内不开新事件(目标在场判定照走, 只抑制
			 * 文件); 低水位同理 —— 两处都必须 continue 跳出触发块,
			 * 只清 g_trigger 不跳出会让块内后续照常开启事件。 */
			if (g_cooldown_until_ms > 0 &&
			    now_ms(CLOCK_MONOTONIC) < g_cooldown_until_ms) {
				g_trigger = 0;
				continue;
			} else if (g_cooldown_until_ms > 0) {
				g_cooldown_until_ms = 0;	/* 冷却结束 */
			}
			if (!disk_allow()) {
				g_trigger = 0;
				continue;	/* 低水位: 不开新事件, 检测统计照走 */
			}
			if (ring_cnt < (int)(pre_sec * 15)) {
				/* 预热未满: 环里的帧数还不够 pre 秒前置内容, 忽略本次
				 * 触发, 但不清 g_streak(攒够后会立刻重新触发) */
				g_trigger = 0;
			} else {
				/* P6: 触发帧 = 本次循环 DQBUF 到的那一帧(检测/触发
				 * 文件/信号都在这一帧的循环里被看到); trig_ts 取该帧
				 * 的环形时间戳(与 json 的 ts_ms 同源), 前后窗口不错位。 */
				trig_seq = frame_seq;
				trig_ts = ring_cnt > 0 ?
					  ring_at(ring_cnt - 1)->ts_ms : ts - t_start;
				/* pre: 从触发点往前按时间回退 pre_sec */
				for (i = ring_cnt - 1; i >= 0; i--)
					if (ring_at(i)->ts_ms <
					    trig_ts - pre_sec * 1000.0)
						break;
				/* 从回退点往前找最近的 IDR(往前才能把 pre 窗口
				 * 填满; 往后找会跳到下一个 GOP 的 IDR, 白丢一段) */
				pre_i = -1;
				for (i = ring_cnt - 1; i >= 0; i--)
					if (ring_at(i)->ts_ms <
					    trig_ts - pre_sec * 1000.0)
						break;
				for (; i >= 0; i--)
					if (ring_at(i)->key) { pre_i = i; break; }
				if (pre_i < 0)
					pre_i = ring_first_idr_from(0);
				g_pre_seq = ring_at(pre_i)->seq;
				/* 触发即"目标在场"锚点: 手动触发模式没有检测命中
				 * 刷新 g_last_detect_ms, 事件靠它 + post 窗口收尾 */
				g_last_detect_ms = now_ms(CLOCK_MONOTONIC);
				printf(">>> 触发 seq=%u ts=%.1fms -> pre_i=%d "
				       "pre_seq=%u (ring_cnt=%d head=%d pushed=%lu)\n",
				       trig_seq, trig_ts, pre_i, g_pre_seq,
				       ring_cnt, ring_head, g_pushed);
				if (evw_open_new(pre_i) == 0) {
					fprintf(stderr,
						"[evw] open event_id=%u last_pkts=%lu "
						"pushed=%lu\n",
						g_ev_event_id, g_ev_last_pkts,
						g_pushed);
				} else {
					fprintf(stderr,
						">>> 事件写入器打开失败, 事件放弃\n");
				}
				/* 触发已消费(成功或失败): 事件延长由
				 * last_detect_ms 判定, 与触发状态无关 */
				g_trigger = 0;
				pre_i = end_i = -1;
				g_pre_seq = 0;
			}
		}

		/* ③ 目标离场: 事件在写, 且最近命中/触发超出 post 窗口 -> 收尾。
		 * S1(docs/37 §3.1): 判据是"事件活跃 + 目标离场", 不是时间锚点;
		 * 检测漏帧不会把持续目标拆成碎事件。 */
		if (g_ev_fp != NULL && g_last_detect_ms > 0 &&
		    now_ms(CLOCK_MONOTONIC) - g_last_detect_ms > post_sec * 1000.0) {
			if (evw_close("target_left", trig_seq, trig_ts) != 0 ||
			    g_ev_werr) {
				fprintf(stderr, ">>> 事件落盘失败\n");
				rc = 1;
				break;
			}
			/* S1(docs/37 §3.4): 不 ring_clear —— ring 照滚, 下一个
			 * 事件的 pre-roll 从这里取; g_streak 不清(目标仍在场时
			 * streak 已在增长, 清零会漏事件)。
			 * S4: 这里是 cooldown 唯一的置位点(target_left 收尾)。 */
			evw_event_end("target_left");
			if (g_max_events && g_events >= g_max_events) {
				reached_max = 1;
				break;
			}
			continue;
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
	/* SIGTERM 且流式事件进行中: 先收尾当前事件(flush+close+rename+JSON,
	 * close_reason=sigterm), 再停 RTSP/线程/采集。S2 后这是唯一的
	 * 在途事件通道 —— 旧 dump 路径已死代码化(S1 移除 ring_clear 后
	 * 只有 g_ev_fp 代表"事件在录")。 */
	if (got_term && g_ev_fp != NULL && !rc) {
		if (evw_close("sigterm", trig_seq, trig_ts) == 0 && !g_ev_werr) {
			printf(">>> SIGTERM: 进行中的事件已落盘 %s\n", g_ev_path);
			fflush(stdout);
		} else {
			fprintf(stderr, ">>> SIGTERM: 事件落盘失败\n");
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
