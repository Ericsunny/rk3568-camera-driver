/* rtsp_hub.c —— evrec 内置 RTSP 服务端+投递环(自 71471d7 的 evrec.c 原样提炼)
 *
 * H1(单客户端行为等价替换): 本文件代码 = 原 evrec.c 行 1400-2520 的逐字拷贝,
 * 仅两处机械替换: g_stop → g_hub_stopping(经 rtsp_hub_set_stop 注入);
 * 其余符号保持 static —— H1 的验收是"行为等价", 多客户端(H3)再重构命名。
 * 设计: docs/38(§3.1a 环所有权 / §4.2a per-client RTP 状态 / §4.3 半开超时)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <dirent.h>
#ifndef SIOCOUTQ
#define SIOCOUTQ 0x5411		/* Linux: 发送队列里尚未确认的字节数 */
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* NAL 参数集缓存: 编码侧 scan 喂入, RTSP DESCRIBE/事件切片读取 */
static pthread_mutex_t g_nal_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_sps[128], g_pps[128];
static int g_sps_len, g_pps_len;

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


#define RTSP_REQ_MAX 4096
#define RTSP_SDP_MAX 2048
static int g_rtsp_port;			/* 监听端口(0=关); evrec --rtsp 解析后经 rtsp_start 传入 */
static int g_rtsp_lfd = -1;
static pthread_t g_rtsp_accept_tid;
static int g_rtsp_accept_started;
static volatile sig_atomic_t g_rtsp_stopping;
static pthread_mutex_t g_rtsp_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_rtsp_cfd = -1;
static int g_rtsp_used;
static int g_rtsp_play;
static int g_rtsp_started;
static int g_rtsp_need_key;
static volatile int g_rtsp_gap;		/* pkb 丢过包: 重置发送游标, 等 IDR */
static int g_rtsp_ch;
static uint16_t g_rtsp_seq;
static uint32_t g_rtsp_ts;
static pthread_t g_rtsp_cli_tid;
static int g_rtsp_cli_started, g_rtsp_cli_joined;
static unsigned long g_rtsp_sent, g_rtsp_drop;
static unsigned long g_rtsp_disc;
static unsigned long g_rtsp_drop_run;
static int g_rtsp_kick;

static volatile sig_atomic_t g_hub_stopping;

void rtsp_hub_set_stop(void)		/* evrec 的信号路径调用 */
{
	g_hub_stopping = 1;
}

void rtsp_hub_notify_recover(void)
{
	pthread_mutex_lock(&g_rtsp_lock);
	g_rtsp_started = 0;
	g_rtsp_need_key = 1;
	pthread_mutex_unlock(&g_rtsp_lock);
}

/* 从 Annex-B 缓冲解析 NAL, 记录 SPS/PPS(与 cam_rtsp.c 相同) */
void rtsp_scan_nals(const uint8_t *d, int n)
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

int rtsp_has_nal(const uint8_t *d, int n, int type)
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
void rtsp_stats_report(void)
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

/* H1: 主循环每帧轮询, hub 内部按 5s/1s 节拍打印(替代原 evrec 侧两个静态计时器) */
static double g_pkb_next_ms = 5000.0;	/* 下一次打印 in/drop 的时间(相对启动) */
static double g_rtsp_next_ms = 1000.0;	/* 聚合日志: 下一次打印(相对启动) */
void rtsp_hub_poll_reports(double now_rel)
{
	while (now_rel >= g_pkb_next_ms) {
		rtsp_ring_report();
		g_pkb_next_ms += 5000.0;
	}
	while (now_rel >= g_rtsp_next_ms) {
		rtsp_stats_report();
		g_rtsp_next_ms += 1000.0;
	}
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
				if (ready || g_rtsp_stopping || g_hub_stopping)
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
	while (!g_rtsp_stopping && !g_hub_stopping) {
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

int rtsp_start(int port)
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
void rtsp_stop(void)
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

/* 链路恢复后由 evrec 调用: 重置发送端状态(等 IDR), 状态本体在 hub 内 */
void rtsp_hub_get_ps(uint8_t *sps, int sps_cap, uint8_t *pps, int pps_cap,
		     int *sl, int *pl)
{
	pthread_mutex_lock(&g_nal_lock);
	*sl = g_sps_len < sps_cap ? g_sps_len : sps_cap;
	*pl = g_pps_len < pps_cap ? g_pps_len : pps_cap;
	if (*sl > 0) memcpy(sps, g_sps, (size_t)*sl);
	if (*pl > 0) memcpy(pps, g_pps, (size_t)*pl);
	pthread_mutex_unlock(&g_nal_lock);
}

void rtsp_hub_set_ring_depth(int n)	{ g_pkb_depth = n; }
void rtsp_hub_set_trace_path(const char *path)	{ g_rtp_trace_path = path; }

/* 编码线程: 只做一次拷贝入环, 环满丢最旧。计数与队列同一把锁, 天然原子。 */
void pkb_push(const unsigned char *src, int len, unsigned seq, int key,
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

/* 把 rtsp_hub_set_stop() 注入的停止状态在锁内同步成 sender 的停止条件 */
void pkb_set_stop(void)
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

void rtsp_ring_report(void)
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

int pkb_start(int depth)
{
	if (depth > 0)
		g_pkb_depth = depth;

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
void pkb_stop(void)
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