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

		if (r->key && g_hdr_len)
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
	printf(">>> 写出 %s.h264 (%d 帧) + %s.json\n", out, n, out);
	return n;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video5", *out = "/root/ev/ev";
	int w = 1920, h = 1080, pre_sec = 3, post_sec = 2, kbps = 8000, gop;
	int i, trigger_i = -1, pre_i = -1, end_i = -1;
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
		if (g_trigger && trigger_i < 0) {
			trigger_i = ring_cnt;
			trig_ts = ts - t_start;
			trig_abs = now_ms(CLOCK_REALTIME);
			/* pre: 从触发点往前按时间回退 pre_sec */
			for (i = ring_cnt - 1; i >= 0; i--)
				if (ring_at(i)->ts_ms < trig_ts - pre_sec * 1000.0)
					break;
			pre_i = ring_first_idr_from(i + 1);
			if (pre_i < 0)
				pre_i = ring_first_idr_from(0);
			printf(">>> 触发 seq=%u ts=%.1fms -> pre_i=%d (丢弃到 IDR)\n",
			       seq, trig_ts, pre_i);
		}
		if (trigger_i >= 0 && ts - t_start > trig_ts + post_sec * 1000.0) {
			end_i = ring_cnt - 1;
			break;
		}
	}

	{
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		xioctl(cap.fd, VIDIOC_STREAMOFF, &t);
	}
	if (pre_i < 0 || end_i < 0) {
		fprintf(stderr, "未触发或无数据, 不落盘\n");
		return 1;
	}
	dump(out, seq - 1, trig_ts, pre_i, end_i, w, h,
	     (double)(end_i - pre_i + 1) * 1000.0 / (trig_ts + post_sec * 1000.0));
	return 0;
}
