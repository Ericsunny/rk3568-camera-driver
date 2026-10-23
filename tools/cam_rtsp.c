/*
 * cam_rtsp.c —— RK3568 单路相机 RTSP 推流(MPP 硬编 + 零依赖 RTSP 服务)
 *
 * 链路: /dev/video5 (NV12, V4L2 mmap) → VEPU H.264 → RTP(FU-A) → RTSP over TCP
 *
 * 板上编译: gcc -O2 -pthread -o cam_rtsp cam_rtsp.c -lrockchip_mpp
 * 运行:     ./cam_rtsp [-d /dev/video5] [-w 1920] [-h 1080] [-r 30] [-b 4000] [-p 8554]
 * 播放:     ffplay rtsp://<ip>:8554/live    或    vlc rtsp://<ip>:8554/live
 *
 * 已知限制(诚实清单):
 *  - 仅 RTSP over TCP(RTP interleaved),不支持 UDP;
 *  - 一次只服务一个客户端(连接断开后可重连);
 *  - 采集线程与编码同线程;客户端不播放时仍然采集编码(丢帧,不缓存)。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <linux/videodev2.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

/* ---------------- 参数 ---------------- */
static const char *g_dev = "/dev/video5";
static int g_w = 1920, g_h = 1080;
static int g_fps = 30;
static int g_kbps = 4000;
static int g_port = 8554;
static int g_http_port = 0;   /* 0 = 关闭浏览器预览 */
static volatile int g_run = 1;
static volatile int g_lfd = -1;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------------- 采集 ---------------- */
struct cap {
	int fd, nbuf, streaming;
	void *map[8];
	size_t len[8];
	uint32_t stride;
	uint32_t sizeimage;
};

static int cap_open(struct cap *c)
{
	struct v4l2_format f;
	struct v4l2_requestbuffers req;
	struct v4l2_capability cp;

	memset(c, 0, sizeof(*c));
	c->fd = open(g_dev, O_RDWR);
	if (c->fd < 0) { fprintf(stderr, "打开 %s 失败: %s\n", g_dev, strerror(errno)); return -1; }
	if (ioctl(c->fd, VIDIOC_QUERYCAP, &cp) < 0) { perror("QUERYCAP"); return -1; }
	printf(">>> 设备 %s (%s)\n", g_dev, cp.card);

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = g_w;
	f.fmt.pix_mp.height = g_h;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (ioctl(c->fd, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); return -1; }
	g_w = f.fmt.pix_mp.width;
	g_h = f.fmt.pix_mp.height;
	c->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	c->sizeimage = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	printf(">>> 采集 %dx%d stride=%u sizeimage=%u\n", g_w, g_h, c->stride, c->sizeimage);

	memset(&req, 0, sizeof(req));
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }
	c->nbuf = req.count;

	for (int i = 0; i < c->nbuf; i++) {
		struct v4l2_buffer b; struct v4l2_plane p;
		memset(&b, 0, sizeof(b)); memset(&p, 0, sizeof(p));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.m.planes = &p; b.length = 1;
		if (ioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return -1; }
		c->len[i] = p.length;
		c->map[i] = mmap(NULL, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, p.m.mem_offset);
		if (c->map[i] == MAP_FAILED) { perror("mmap"); return -1; }
		if (ioctl(c->fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return -1; }
	}
	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(c->fd, VIDIOC_STREAMON, &t) < 0) { perror("STREAMON"); return -1; }
	c->streaming = 1;
	return 0;
}

static void cap_close(struct cap *c)
{
	if (c->streaming) {
		int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		ioctl(c->fd, VIDIOC_STREAMOFF, &t);
		c->streaming = 0;
	}
	for (int i = 0; i < c->nbuf; i++)
		if (c->map[i] && c->map[i] != MAP_FAILED) munmap(c->map[i], c->len[i]);
	if (c->fd > 0) close(c->fd);
}


/* 取一帧。返回 mmap 地址;成功 0。 */
static int cap_grab(struct cap *c, void **addr, size_t *bytes)
{
	struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
	if (poll(&pfd, 1, 3000) <= 0) return -1;

	struct v4l2_buffer b; struct v4l2_plane p;
	memset(&b, 0, sizeof(b)); memset(&p, 0, sizeof(p));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &p; b.length = 1;
	if (ioctl(c->fd, VIDIOC_DQBUF, &b) < 0) {
		if (errno != EAGAIN) perror("DQBUF");
		return -1;
	}
	*addr = c->map[b.index];
	*bytes = c->len[b.index];
	return 0;
}

/* 还回缓冲(在拷贝进编码器之后调用) */
static int cap_return(struct cap *c, void *addr)
{
	int idx = -1;
	for (int i = 0; i < c->nbuf; i++) if (c->map[i] == addr) { idx = i; break; }
	if (idx < 0) return -1;
	struct v4l2_buffer b; struct v4l2_plane p;
	memset(&b, 0, sizeof(b)); memset(&p, 0, sizeof(p));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = idx; b.m.planes = &p; b.length = 1;
	return ioctl(c->fd, VIDIOC_QBUF, &b) < 0 ? -1 : 0;
}

/* ---------------- 编码 ---------------- */
struct enc {
	MppCtx ctx; MppApi *mpi; MppBufferGroup grp; MppBuffer buf;
	int ok;
};

static int enc_init(struct enc *e)
{
	MppEncCfg cfg;
	MPP_RET r;

	if (mpp_create(&e->ctx, &e->mpi) != MPP_OK) return -1;
	if (mpp_init(e->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return -1;

	mpp_enc_cfg_init(&cfg);
	mpp_enc_cfg_set_s32(cfg, "prep:width", g_w);
	mpp_enc_cfg_set_s32(cfg, "prep:height", g_h);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", g_w);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", g_h);
	mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_MPEG);

	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_target", g_kbps * 1000);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", g_kbps * 1000 * 17 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", g_kbps * 1000 * 15 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", g_fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", g_fps);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:gop", g_fps * 2);

	mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
	mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
	mpp_enc_cfg_set_s32(cfg, "h264:level", 51);
	mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
	mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

	r = e->mpi->control(e->ctx, MPP_ENC_SET_CFG, cfg);
	/* cfg 由 MPP 内部管理,不手动释放 */
	if (r != MPP_OK) { fprintf(stderr, "ENC_SET_CFG 失败 %d\n", r); return -1; }

	mpp_buffer_group_get_internal(&e->grp, MPP_BUFFER_TYPE_ION);
	if (mpp_buffer_get(e->grp, &e->buf, (size_t)g_w * g_h * 3 / 2) != MPP_OK) {
		fprintf(stderr, "mpp_buffer_get 失败\n"); return -1;
	}
	e->ok = 1;
	return 0;
}

/* 把一帧 NV12(可能 stride 大于宽)拷进编码器缓冲 */
static void copy_nv12(uint8_t *dst, const uint8_t *src, int w, int h, uint32_t stride)
{
	if (stride == (uint32_t)w) {
		memcpy(dst, src, (size_t)w * h * 3 / 2);
		return;
	}
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * w, src + (size_t)y * stride, w);
	uint8_t *d2 = dst + (size_t)w * h;
	const uint8_t *s2 = src + (size_t)stride * h;
	for (int y = 0; y < h / 2; y++)
		memcpy(d2 + (size_t)y * w, s2 + (size_t)y * stride, w);
}

/* ---------------- 帧环 ---------------- */
#define RING 8
struct eframe {
	uint8_t *data;
	int len, key, ready;
	uint64_t pts_ms;
};
static struct eframe g_ring[RING];
static int g_wr, g_rd;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_have_client;

static uint8_t g_sps[128], g_pps[128];
static int g_sps_len, g_pps_len;

/* 从 Annex-B 缓冲解析 NAL,记录 SPS/PPS */
static void scan_nals(const uint8_t *d, int n)
{
	int i = 0;
	while (i + 3 < n) {
		int sc = 0;
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) sc = 3;
		else if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) sc = 4;
		if (!sc) { i++; continue; }
		int st = i + sc;
		int j = st;
		while (j + 3 < n) {
			if (d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 ||
			    (d[j + 2] == 0 && d[j + 3] == 1))) break;
			j++;
		}
		if (j + 3 >= n) j = n;
		int len = j - st;
		if (len > 0) {
			int t = d[st] & 0x1f;
			if (t == 7 && len < (int)sizeof(g_sps)) { memcpy(g_sps, d + st, len); g_sps_len = len; }
			if (t == 8 && len < (int)sizeof(g_pps)) { memcpy(g_pps, d + st, len); g_pps_len = len; }
		}
		i = j;
	}
}

/* 是否含 IDR 片(type 5) */
static int has_idr(const uint8_t *d, int n)
{
	for (int i = 0; i + 4 < n; i++) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1f) == 5)
			return 1;
	}
	return 0;
}

/* ---------------- RTP 发送 ---------------- */
static int send_interleaved(int sock, int ch, const uint8_t *rtp, int len)
{
	uint8_t pre[4] = { '$', (uint8_t)ch, (uint8_t)(len >> 8), (uint8_t)(len & 0xff) };
	struct iovec iov[2] = { { pre, 4 }, { (void *)rtp, (size_t)len } };
	struct msghdr mh = { .msg_iov = iov, .msg_iovlen = 2 };
	return sendmsg(sock, &mh, 0) < 0 ? -1 : 0;
}

static int send_nal(int sock, int ch, const uint8_t *nal, int len, uint32_t ts,
		    uint16_t *seq, int marker)
{
	const int MAXP = 1400;
	if (len <= MAXP) {
		uint8_t pkt[12 + MAXP];
		memset(pkt, 0, 12);
		pkt[0] = 0x80; pkt[1] = 96 | (marker ? 0x80 : 0);
		pkt[2] = *seq >> 8; pkt[3] = *seq & 0xff;
		pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
		memcpy(pkt + 12, nal, len);
		(*seq)++;
		return send_interleaved(sock, ch, pkt, 12 + len);
	}
	int off = 1;
	int first = 1;
	while (off < len) {
		int n = len - off;
		if (n > MAXP - 2) n = MAXP - 2;
		int end = (off + n >= len);
		uint8_t pkt[12 + MAXP];
		memset(pkt, 0, 12);
		pkt[0] = 0x80;
		pkt[1] = 96 | ((end && marker) ? 0x80 : 0);
		pkt[2] = *seq >> 8; pkt[3] = *seq & 0xff;
		pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
		pkt[12] = (nal[0] & 0xe0) | 28;              /* FU indicator */
		pkt[13] = (nal[0] & 0x1f) | (first ? 0x80 : 0) | (end ? 0x40 : 0);
		memcpy(pkt + 14, nal + off, n);
		(*seq)++;
		if (send_interleaved(sock, ch, pkt, 14 + n) < 0) return -1;
		off += n;
		first = 0;
	}
	return 0;
}

/* 把整帧(Annex-B,多个 NAL)按 RTP 发出;最后一个包打 marker */
static int rtp_send_frame(int sock, int ch, struct eframe *f, uint32_t ts, uint16_t *seq)
{
	struct { const uint8_t *p; int len; } nal[40];
	int nn = 0;
	const uint8_t *d = f->data;
	int n = f->len, i = 0;

	while (i + 3 < n && nn < 40) {
		int sc = 0;
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) sc = 3;
		else if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) sc = 4;
		if (!sc) { i++; continue; }
		int st = i + sc, j = st;
		while (j + 3 < n) {
			if (d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 ||
			    (d[j + 2] == 0 && d[j + 3] == 1))) break;
			j++;
		}
		if (j + 3 >= n) j = n;
		if (j - st > 0) { nal[nn].p = d + st; nal[nn].len = j - st; nn++; }
		i = j;
	}
	if (!nn) return 0;

	for (int k = 0; k < nn; k++) {
		int is_last = (k == nn - 1);
		int type = nal[k].p[0] & 0x1f; (void)type;
		/* 关键帧先补 SPS/PPS,保证中途接入的播放器能解码 */
		if (k == 0 && f->key && g_sps_len && g_pps_len) {
			if (send_nal(sock, ch, g_sps, g_sps_len, ts, seq, 0) < 0) return -1;
			if (send_nal(sock, ch, g_pps, g_pps_len, ts, seq, 0) < 0) return -1;
		}
		if (send_nal(sock, ch, nal[k].p, nal[k].len, ts, seq, is_last) < 0) return -1;
	}
	return 0;
}

/* ---------------- 采集+编码线程 ---------------- */
static void sig_handler(int sig)
{
	(void)sig;
	g_run = 0;
	/* 打断 accept():SA_RESTART 下 accept 会自动重启,必须主动关掉监听 fd */
	if (g_lfd >= 0) {
		close((int)g_lfd);
		g_lfd = -1;
	}
}

/* ---------------- 浏览器预览(HTTP + BMP 快照) ---------------- */
#define SNAP_W 640
#define SNAP_H 360
static uint8_t *g_bmp;
static int g_bmp_len;
static pthread_mutex_t g_bmp_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

/* NV12 -> BMP(640x360,BT.601 limited range),供浏览器 <img> 拉取 */
static void snap_update(const uint8_t *nv12, int w, int h, int stride)
{
	int rowsz = (SNAP_W * 3 + 3) & ~3;
	int len = 54 + rowsz * SNAP_H;
	uint8_t *b = malloc(len);
	if (!b) return;
	memset(b, 0, 54);
	b[0] = 'B'; b[1] = 'M';
	*(uint32_t *)(b + 2) = (uint32_t)len;
	*(uint32_t *)(b + 10) = 54;
	*(uint32_t *)(b + 14) = 40;
	*(int32_t *)(b + 18) = SNAP_W;
	*(int32_t *)(b + 22) = SNAP_H;
	*(uint16_t *)(b + 26) = 1;
	*(uint16_t *)(b + 28) = 24;
	*(uint32_t *)(b + 34) = (uint32_t)(rowsz * SNAP_H);
	const uint8_t *uv = nv12 + (size_t)stride * h;
	for (int y = 0; y < SNAP_H; y++) {
		int sy = y * h / SNAP_H;
		uint8_t *row = b + 54 + (size_t)(SNAP_H - 1 - y) * rowsz;
		for (int x = 0; x < SNAP_W; x++) {
			int sx = x * w / SNAP_W;
			int Y = nv12[(size_t)sy * stride + sx] - 16;
			int U = uv[(size_t)(sy / 2) * stride + (sx & ~1)] - 128;
			int V = uv[(size_t)(sy / 2) * stride + (sx & ~1) + 1] - 128;
			row[x * 3 + 0] = clamp8((298 * Y + 516 * U + 128) >> 8);
			row[x * 3 + 1] = clamp8((298 * Y - 100 * U - 208 * V + 128) >> 8);
			row[x * 3 + 2] = clamp8((298 * Y + 409 * V + 128) >> 8);
		}
	}
	pthread_mutex_lock(&g_bmp_lock);
	free(g_bmp); g_bmp = b; g_bmp_len = len;
	pthread_mutex_unlock(&g_bmp_lock);
}

static const char g_page[] =
"HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n\r\n"
"<html><head><meta charset='utf-8'><title>RK3568 camera</title></head>"
"<body style='margin:0;background:#111'>"
"<div style='color:#ddd;font:14px sans-serif;padding:8px'>RK3568 实时预览(约 5 fps 快照)</div>"
"<img id=v style='width:100%;max-width:960px;display:block'>"
"<script>var t=0;function f(){document.getElementById('v').src='/snap.bmp?t='+(t++);setTimeout(f,200);}f();</script>"
"</body></html>";

static void *http_thread(void *arg)
{
	int port = (int)(intptr_t)arg;
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	if (lfd < 0) return NULL;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port),
				  .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("http bind"); return NULL; }
	listen(lfd, 8);
	printf(">>> 浏览器预览: http://192.168.10.2:%d/  用浏览器打开\n", port);
	fflush(stdout);
	while (g_run) {
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0) { if (!g_run) break; continue; }
		char req[512] = {0};
		ssize_t r = read(fd, req, sizeof(req) - 1);
		(void)r;
		if (strstr(req, "GET /snap.bmp")) {
			pthread_mutex_lock(&g_bmp_lock);
			int len = g_bmp_len;
			uint8_t *tmp = len ? malloc(len) : NULL;
			if (tmp) memcpy(tmp, g_bmp, len);
			pthread_mutex_unlock(&g_bmp_lock);
			if (tmp) {
				char hdr[192];
				int hl = snprintf(hdr, sizeof(hdr),
					"HTTP/1.0 200 OK\r\nContent-Type: image/bmp\r\n"
					"Content-Length: %d\r\nCache-Control: no-store\r\n\r\n", len);
				if (write(fd, hdr, hl) < 0) { /* ignore */ }
				if (write(fd, tmp, len) < 0) { /* ignore */ }
				free(tmp);
			} else {
				const char *e = "HTTP/1.0 503 Busy\r\nContent-Length: 0\r\n\r\n";
				if (write(fd, e, strlen(e)) < 0) { /* ignore */ }
			}
		} else {
			if (write(fd, g_page, sizeof(g_page) - 1) < 0) { /* ignore */ }
		}
		close(fd);
	}
	close(lfd);
	return NULL;
}

static void *capture_thread(void *arg)
{
	struct cap *c = arg;
	struct enc e;
	if (enc_init(&e) < 0) { g_run = 0; return NULL; }

	uint64_t idx = 0;
	double t0 = now_ms();
	int nf = 0;
	while (g_run) {
		void *addr; size_t bytes;
		if (cap_grab(c, &addr, &bytes) < 0) continue;

		uint8_t *dst = mpp_buffer_get_ptr(e.buf);
		copy_nv12(dst, addr, g_w, g_h, c->stride);
		if (g_http_port && (idx % 3) == 0) snap_update(dst, g_w, g_h, g_w);
		cap_return(c, addr);

		MppFrame fr = NULL;
		mpp_frame_init(&fr);
		mpp_frame_set_width(fr, g_w);
		mpp_frame_set_height(fr, g_h);
		mpp_frame_set_hor_stride(fr, g_w);
		mpp_frame_set_ver_stride(fr, g_h);
		mpp_frame_set_fmt(fr, MPP_FMT_YUV420SP);
		mpp_frame_set_buffer(fr, e.buf);
		if (e.mpi->encode_put_frame(e.ctx, fr) != MPP_OK) { mpp_frame_deinit(&fr); continue; }

		MppPacket pk = NULL;
		if (e.mpi->encode_get_packet(e.ctx, &pk) != MPP_OK || !pk) {
			mpp_frame_deinit(&fr); continue;
		}
		int plen = mpp_packet_get_length(pk);
		void *pdata = mpp_packet_get_data(pk);
		int key = has_idr(pdata, plen);

		scan_nals(pdata, plen);

		if (g_have_client) {
			pthread_mutex_lock(&g_lock);
			struct eframe *ef = &g_ring[g_wr % RING];
			if (ef->data) { free(ef->data); ef->data = NULL; }
			ef->data = malloc(plen);
			if (ef->data) {
				memcpy(ef->data, pdata, plen);
				ef->len = plen; ef->key = key; ef->ready = 1;
				ef->pts_ms = idx * 1000 / g_fps;
				g_wr++;
				pthread_cond_signal(&g_cond);
			}
			pthread_mutex_unlock(&g_lock);
		}

		mpp_packet_deinit(&pk);
		mpp_frame_deinit(&fr);
		idx++; nf++;
		if (nf % 60 == 0) {
			double dt = (now_ms() - t0) / 1000.0;
			printf(">>> 采集 %d 帧, %.1f fps\n", nf, nf / dt);
			fflush(stdout);
		}
	}
	printf(">>> 采集线程退出(共 %d 帧)\n", nf);
	return NULL;
}

/* ---------------- RTSP ---------------- */
static const char *b64(const uint8_t *d, int n, char *out)
{
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

static int read_req(int fd, char *buf, int cap)
{
	int n = 0;
	while (n < cap - 1) {
		int r = read(fd, buf + n, cap - 1 - n);
		if (r <= 0) return -1;
		n += r;
		buf[n] = 0;
		if (strstr(buf, "\r\n\r\n")) return n;
	}
	return n;
}

static void serve_client(int fd)
{
	char req[4096];
	char sdp[1024];
	char b64s[512], b64p[512];
	int ch_rtp = 0, ch_rtcp = 1;
	char session[32] = "12345678";
	int cseq = 0;
	uint32_t ts = 0;
	uint16_t seq = 0;
	int rd = 0;

	while (g_run) {
		if (read_req(fd, req, sizeof(req)) <= 0) break;

		/* CSeq */
		char *p = strstr(req, "CSeq:");
		if (p) sscanf(p + 5, "%d", &cseq);

		if (strncmp(req, "OPTIONS", 7) == 0) {
			const char *r =
				"RTSP/1.0 200 OK\r\n"
				"Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n";
			char out[256];
			snprintf(out, sizeof(out), "%sCSeq: %d\r\n\r\n", r, cseq);
			if (write(fd, out, strlen(out)) < 0) break;
		} else if (strncmp(req, "DESCRIBE", 8) == 0) {
			if (!g_sps_len || !g_pps_len) {
				const char *e = "RTSP/1.0 503 Service Unavailable\r\nCSeq: 0\r\n\r\n";
				write(fd, e, strlen(e));
				continue;
			}
			b64(g_sps, g_sps_len, b64s);
			b64(g_pps, g_pps_len, b64p);
			snprintf(sdp, sizeof(sdp),
				 "v=0\r\n"
				 "o=- 0 0 IN IP4 127.0.0.1\r\n"
				 "s=RK3568 Camera\r\n"
				 "c=IN IP4 0.0.0.0\r\n"
				 "t=0 0\r\n"
				 "a=control:*\r\n"
				 "m=video 0 RTP/AVP 96\r\n"
				 "a=rtpmap:96 H264/90000\r\n"
				 "a=fmtp:96 packetization-mode=1;profile-level-id=640028;"
				 "sprop-parameter-sets=%s,%s\r\n"
				 "a=control:trackID=0\r\n",
				 b64s, b64p);
			char out[2048];
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
				 "Content-Base: rtsp://127.0.0.1:%d/live/\r\n"
				 "Content-Type: application/sdp\r\n"
				 "Content-Length: %d\r\n\r\n%s",
				 cseq, g_port, (int)strlen(sdp), sdp);
			if (write(fd, out, strlen(out)) < 0) break;
		} else if (strncmp(req, "SETUP", 5) == 0) {
			char *tr = strstr(req, "interleaved=");
			if (tr) sscanf(tr + 12, "%d-%d", &ch_rtp, &ch_rtcp);
			char out[512];
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
				 "Session: %s\r\n"
				 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=12345678\r\n\r\n",
				 cseq, session, ch_rtp, ch_rtcp);
			if (write(fd, out, strlen(out)) < 0) break;
		} else if (strncmp(req, "PLAY", 4) == 0) {
			char out[512];
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n"
				 "Range: npt=0.000-\r\n"
				 "RTP-Info: url=rtsp://127.0.0.1:%d/live/trackID=0;seq=%u;rtptime=%u\r\n\r\n",
				 cseq, session, g_port, seq, ts);
			if (write(fd, out, strlen(out)) < 0) break;
			rd = 1;
			pthread_mutex_lock(&g_lock);
			g_have_client = 1;
			pthread_mutex_unlock(&g_lock);

			/* 进入发送循环 */
			uint64_t sent_ts = 0;
			while (g_run && rd) {
				struct eframe *fr = NULL;
				pthread_mutex_lock(&g_lock);
				int spins = 0;
				while (g_rd == g_wr && g_run && spins++ < 200) {
					struct timespec to = { 0, 100 * 1000 * 1000 };
					pthread_cond_timedwait(&g_cond, &g_lock, &to);
					/* 期间可能客户端断开,靠 write 失败退出 */
					if (g_rd == g_wr) break;
				}
				if (g_rd != g_wr) {
					fr = &g_ring[g_rd % RING];
					g_rd++;
				}
				pthread_mutex_unlock(&g_lock);

				if (fr && fr->ready) {
					uint32_t rts = (uint32_t)(fr->pts_ms * 90);
					uint16_t sq = seq;
					if (rtp_send_frame(fd, ch_rtp, fr, rts, &sq) < 0) {
						fprintf(stderr, ">>> 发送失败,客户端断开\n");
						goto done;
					}
					seq = sq;
					ts = rts;
					sent_ts++;
					/* 标记位:重发最后包以设置 marker 太复杂,这里改为在
					 * rtp_send_frame 内对最后一个 NAL 的最后一包设置。已实现。*/
					pthread_mutex_lock(&g_lock);
					if (fr->data) { free(fr->data); fr->data = NULL; }
					fr->ready = 0;
					pthread_mutex_unlock(&g_lock);
				}
			}
		} else if (strncmp(req, "TEARDOWN", 8) == 0) {
			const char *r = "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n";
			char out[128];
			snprintf(out, sizeof(out), r, cseq);
			write(fd, out, strlen(out));
			break;
		} else if (strncmp(req, "GET_PARAMETER", 13) == 0) {
			char out[128];
			snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseq);
			if (write(fd, out, strlen(out)) < 0) break;
		} else {
			const char *r = "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n";
			char out[128];
			snprintf(out, sizeof(out), r, cseq);
			if (write(fd, out, strlen(out)) < 0) break;
		}
	}
done:
	pthread_mutex_lock(&g_lock);
	g_have_client = 0;
	pthread_mutex_unlock(&g_lock);
	close(fd);
	fprintf(stderr, ">>> 客户端断开\n");
}

int main(int argc, char **argv)
{
	int opt;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	while ((opt = getopt(argc, argv, "d:w:h:r:b:p:H:")) != -1) {
		switch (opt) {
		case 'd': g_dev = optarg; break;
		case 'w': g_w = atoi(optarg); break;
		case 'h': g_h = atoi(optarg); break;
		case 'r': g_fps = atoi(optarg); break;
		case 'b': g_kbps = atoi(optarg); break;
		case 'p': g_port = atoi(optarg); break;
		case 'H': g_http_port = atoi(optarg); break;
		default:
			fprintf(stderr, "用法: %s [-d dev] [-w W] [-h H] [-r fps] [-b kbps] [-p port]\n", argv[0]);
			return 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sact;
	memset(&sact, 0, sizeof(sact));
	sact.sa_handler = sig_handler;
	sigaction(SIGINT, &sact, NULL);
	sigaction(SIGTERM, &sact, NULL);

	struct cap c;
	if (cap_open(&c) < 0) return 1;

	pthread_t th;
	pthread_create(&th, NULL, capture_thread, &c);

	/* 等 SPS/PPS 就绪 */
	double t0 = now_ms();
	while (!g_sps_len && now_ms() - t0 < 5000) usleep(50000);

	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(g_port),
				  .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
	listen(lfd, 4);
	g_lfd = lfd;
	printf(">>> RTSP 服务已就绪: rtsp://192.168.10.2:%d/live  (SPS=%d PPS=%d)\n",
	       g_port, g_sps_len, g_pps_len);
	fflush(stdout);

	if (g_http_port) {
		pthread_t ht;
		pthread_create(&ht, NULL, http_thread, (void *)(intptr_t)g_http_port);
	}

	while (g_run) {
		struct sockaddr_in cli;
		socklen_t cl = sizeof(cli);
		int fd = accept(lfd, (struct sockaddr *)&cli, &cl);
		if (fd < 0) {
			if (!g_run) break;
			if (errno == EINTR) continue;
			break;
		}
		one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		printf(">>> 客户端接入 %s:%d\n", inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));
		fflush(stdout);
		serve_client(fd);
	}

	g_run = 0;
	pthread_mutex_lock(&g_lock);
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);
	pthread_join(th, NULL);
	cap_close(&c);
	close(lfd);
	return 0;
}
