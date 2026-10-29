/* rtsp_hub.c —— evrec 的 RTSP 服务端+分发环(H1 自 evrec.c 提炼, H3 多客户端化)
 *
 * H1: 单客户端行为等价替换(已验收, evidence/2026-10-28-H1关闭验证/)。
 * H3(docs/38): 每连接一线程(信令+发送同线程, cam_rtsp 模型), 共享分发环+
 *     每客户端私有游标; A1 锁内 memcpy 到私有缓冲 / A2 per-client seq·ssrc·
 *     marker / A3 SETUP 后 10s 半开超时; PLAY 名额满回 453+Retry-After(B2);
 *     B4 链路恢复清环+epoch; B3 环双上限(包数+字节); B5 每客户端统计口径。
 * g_stop → g_hub_stopping 的注入方式保持 H1 不变。
 */
#define _GNU_SOURCE		/* memmem(glibc 只在 _GNU_SOURCE 下声明) */
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
#include <time.h>
#include <dirent.h>

#include "rtsp_hub.h"		/* 包含自身接口: 原型/定义不匹配在编译期暴露 */
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
	TX_WHY_STALL,		/* deadline 秒零进展(H4 §4.4): 客户端不收 */
};
#define RTSP_SNDBUF_REQ (512 * 1024)	/* 内核按 wmem_max 封顶, 实际 >= 2*wmem_max */
#define RTSP_TX_POLL_MS 100		/* 单次等可写的间隔(H4 起不再有 10 次上限) */
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


static int g_rtsp_port;			/* 监听端口(0=关); evrec --rtsp 解析后经 rtsp_start 传入 */
static int g_rtsp_lfd = -1;
static pthread_t g_rtsp_accept_tid;
static int g_rtsp_accept_started;
static volatile sig_atomic_t g_rtsp_stopping;
static pthread_mutex_t g_rtsp_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t g_hub_stopping;

/* ---- 分发环(H3): 生产者写, N 个客户端按各自游标消费 ----
 * 环内 tag 连续: [oldest, next), 物理槽 = tag % PKB_MAX。条目归环所有,
 * 被逐出才 free —— 消费端按 A1 在环锁内 memcpy 到私有缓冲。 */
#define PKB_MAX 64		/* --rtsp-ring 的硬上限 */
struct pktbuf {
	unsigned char *data;
	int len;
	unsigned seq;
	unsigned char key;
	uint32_t ts90;		/* 90kHz RTP 时间戳, 编码线程按源帧时间算好 */
	uint64_t tag;		/* 绝对序号: 单调递增, 客户端游标即 tag */
};

static struct pktbuf g_pkb[PKB_MAX];
static int g_pkb_depth = 30;	/* --rtsp-ring N(包数), 默认 30(docs/38 §7a B3) */
static int g_pkb_mb = 8;	/* --rtsp-ring-mb(字节上限 MB), 字节优先构成内存上界 */
static uint64_t g_pkb_oldest_tag, g_pkb_next_tag;	/* 空环: oldest == next */
static unsigned long g_pkb_bytes;	/* 环内字节总量(B3) */
static unsigned long g_pkb_in, g_pkb_drop;	/* drop = 被逐出的包(hub 级) */
static int g_pkb_stop_local;
static pthread_mutex_t g_pkb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pkb_cond = PTHREAD_COND_INITIALIZER;
static int g_pkb_started;

/* ---- 客户端表(H3): 每连接一线程, 信令与发送在同一线程(cam_rtsp 模型) ----
 * A1: 消费 = 环锁内 memcpy 到私有缓冲, 锁外只碰私有缓冲;
 * A2: seq/ssrc/ch 状态全在客户端结构里, 打包函数经 rtsp_cli* 传入;
 * A3: SETUP 后 10s 未 PLAY 主动 close(半开超时, 经 read_req 的 deadline)。
 * 槽生命周期: accept 占槽(used=1) → 线程退出标 th_done → join 后 used=0
 * (槽复用以 join 完成为前提, 防 pthread 栈泄漏)。
 * 槽表 10 = max_clients 上限 8 + 2 个半开余量(docs/38 §4.3)。 */
#define RTSP_CLI_MAX 10
struct rtsp_cli {
	int used;		/* 槽被占用(连接存活, join 后清 0) */
	int playing;		/* PLAY 已接受(占用播放名额) */
	int th_done;		/* 线程已退出, 待 join */
	int joined;
	pthread_t tid;
	int fd;			/* -1 = 本线程已关 */
	char session[32];
	int ch;			/* interleaved RTP 通道 */
	uint16_t seq;		/* RTP 序号(16 位回绕), per-client(A2) */
	uint32_t ssrc;		/* SETUP 时生成, per-client(A2) */
	uint64_t cursor;	/* 下一个待发包的绝对 tag(判定持环锁) */
	int need_key;		/* 等下一个 IDR(起播/跳帧/被逐出后) */
	uint8_t *sbuf;		/* 私有帧缓冲(A1), 按需增长 */
	int scap;
	/* B5 统计口径: 记包不记帧; 被动丢(evicted)与主动跳(skip)分开 */
	unsigned long sent_pkts;	/* RTP 包数(不是帧) */
	unsigned long sent_bytes;	/* RTP 负载字节(Annex-B 帧长合计) */
	unsigned long skip_frames;	/* 主动跳帧(need_key 同步跳过) */
	unsigned long evicted;		/* 被生产者逐出(被动丢) */
	unsigned long sock_dead;	/* 发送判死 */
	uint64_t last_send_ms;		/* 最后成功发送时刻(CLOCK_MONOTONIC ms) */
};

static struct rtsp_cli g_cli[RTSP_CLI_MAX];
static int g_max_clients = 4;		/* --rtsp-clients, clamp [1,8] */
static int g_deadline_s = 5;		/* --rtsp-deadline-s(§4.4 硬上限), clamp [1,120] */
static uint32_t g_ts_off;		/* --ts-offset: RTP ts 注入偏移(回绕测试), 0=关 */
static unsigned long g_rej453;		/* 453/槽满拒绝次数(容量指标, B2) */
static unsigned long g_rtsp_disc;	/* 对端主动断开(EPIPE/ECONNRESET) */
static unsigned long g_hub_epoch;	/* B4: 链路恢复次数(notify_recover ++) */

static uint64_t now_ms_mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static const char *tx_why_name(int why)
{
	static const char * const names[] = { "ok", "precheck", "eagain",
					      "error", "short", "stall" };

	return (why >= 0 && why < (int)(sizeof(names) / sizeof(names[0]))) ?
	       names[why] : "?";
}

static int count_playing_locked(void)	/* 调用者持 g_rtsp_lock */
{
	int i, n = 0;

	for (i = 0; i < RTSP_CLI_MAX; i++)
		if (g_cli[i].used && g_cli[i].playing)
			n++;
	return n;
}

/* 以下两个只在持有 g_pkb_lock 时调用 */
static uint64_t pkb_find_key_from(uint64_t from)
{
	uint64_t t;

	if (from < g_pkb_oldest_tag)
		from = g_pkb_oldest_tag;
	for (t = from; t < g_pkb_next_tag; t++)
		if (g_pkb[t % PKB_MAX].key)
			return t;
	return g_pkb_next_tag;
}

static uint64_t pkb_latest_key(void)
{
	uint64_t t = g_pkb_next_tag;

	while (t > g_pkb_oldest_tag) {
		t--;
		if (g_pkb[t % PKB_MAX].key)
			return t;
	}
	return g_pkb_next_tag;
}

void rtsp_hub_set_stop(void)		/* evrec 的信号路径调用 */
{
	g_hub_stopping = 1;
}

/* B4: 链路恢复后 epoch++ 并清环 —— 客户端不消费故障前的旧包, 各端
 * need_key=1, 从恢复后的下一个 IDR 重新起步(docs/38 §7a)。 */
void rtsp_hub_notify_recover(void)
{
	unsigned char *victims[PKB_MAX];
	int i, nv = 0;

	pthread_mutex_lock(&g_pkb_lock);
	while (g_pkb_oldest_tag < g_pkb_next_tag) {
		struct pktbuf *p = &g_pkb[g_pkb_oldest_tag % PKB_MAX];

		if (nv < PKB_MAX)
			victims[nv++] = p->data;
		p->data = NULL;
		g_pkb_oldest_tag++;
	}
	g_pkb_bytes = 0;
	pthread_mutex_unlock(&g_pkb_lock);
	for (i = 0; i < nv; i++)
		free(victims[i]);

	pthread_mutex_lock(&g_rtsp_lock);
	g_hub_epoch++;
	for (i = 0; i < RTSP_CLI_MAX; i++)
		if (g_cli[i].used)
			g_cli[i].need_key = 1;
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
 *   - EAGAIN 用 poll(POLLOUT) 有界等待; H4(§4.4)起判死条件不再是"单包
 *     等 1s"而是"连续 --rtsp-deadline-s(默认 5)秒零进展": 自首次零进展
 *     起计时, 期间任何字节推进都清零重来 —— 3s 停读的客户端活得到恢复
 *     (V3 判据), 超过 deadline 才主动 close;
 *   - EPIPE/ECONNRESET/对端关闭 → FATAL, 上层 shutdown 该连接;
 *     "半个访问单元留在线上" 比断开严重得多, 所以帧内失败只能断连。
 * 所有写都发生在 g_rtsp_lock 里(单写者), 帧内不可能有并发写。
 */
static int rtsp_send_interleaved(int fd, int ch, const uint8_t *rtp, int len,
				 struct txdiag *dg)
{
	/* 4 + RTP: RTP 头 12 + 负载 <= 1400, 连续缓冲区让短写可以简单续写 */
	uint8_t buf[4 + 12 + 1400];
	int total, off = 0, outq = 0, sndbuf = 0;
	uint64_t stall_anchor = 0;	/* H4: 首次零进展时刻(0=无进展阻塞) */

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
			stall_anchor = 0;	/* 有进展: H4 零进展时钟清零 */
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
		/* 队列满: §4.4 硬上限 —— 自首次零进展最多等 g_deadline_s 秒,
		 * 到点判死; 期间任何推进已在上面把锚点清零(V3: 3s 停读存活) */
		if (stall_anchor == 0) {
			stall_anchor = now_ms_mono();
		} else if (now_ms_mono() - stall_anchor >=
			   (uint64_t)g_deadline_s * 1000) {
			if (dg) {
				dg->why = TX_WHY_STALL;
				dg->w = off;
				dg->err = EAGAIN;
			}
			return RTSP_TX_FATAL;	/* deadline 秒零进展: 判死 */
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

/* A2: RTP 状态(seq)全部经 rtsp_cli* 传入, 无 g_ 全局(docs/38 §4.2a) */
static int rtsp_send_nal(struct rtsp_cli *c, const uint8_t *nal, int len,
			 uint32_t ts, int marker, struct txdiag *dg)
{
	const int MAXP = 1400;
	int rc;

	if (len <= MAXP) {
		uint8_t pkt[12 + MAXP];
		uint16_t sq = c->seq;

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
		c->seq++;
		rc = rtsp_send_interleaved(c->fd, c->ch, pkt, 12 + len, dg);
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
			uint16_t sq = c->seq;

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
			c->seq++;
			/* 一片发不出去只能是连接坏了(整包要么全进内核, 要么
			 * 有界等待后 FATAL): 返回 FATAL 让上层立刻 shutdown,
			 * 绝不把缺了后半段的 FU-A 留在线上 */
			rc = rtsp_send_interleaved(c->fd, c->ch, pkt, 14 + n, dg);
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
static int rtsp_send_frame(struct rtsp_cli *c, const uint8_t *d, int n,
			   int key, uint32_t ts, struct txdiag *dg)
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
			rc = rtsp_send_nal(c, sps, sl, ts, 0, dg);
			if (rc != RTSP_TX_OK)
				return rc;
		}
		if (pl > 0 && !rtsp_has_nal(d, n, 8)) {
			rc = rtsp_send_nal(c, pps, pl, ts, 0, dg);
			if (rc != RTSP_TX_OK)
				return rc;
		}
	}
	for (k = 0; k < nn; k++) {
		rc = rtsp_send_nal(c, nal[k].p, nal[k].len, ts,
				   k == nn - 1, dg);
		if (rc != RTSP_TX_OK)
			return rc;
	}
	return RTSP_TX_OK;
}

/* H3: 单客户端的 sender 线程/rtsp_send_packet 已删除 —— 每个客户端线程
 * 自己从分发环按私有游标取包发送(见 rtsp_client_thread 的发送循环)。 */

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

/* 每秒一行的聚合日志: hub 行 + 每个 PLAY 中客户端一行(B5 字段) */
void rtsp_stats_report(void)
{
	int i, conn = 0, play = 0, fds;
	unsigned long sent = 0, rej, ep;
	uint64_t now = now_ms_mono();

	pthread_mutex_lock(&g_rtsp_lock);
	rej = g_rej453;
	ep = g_hub_epoch;
	for (i = 0; i < RTSP_CLI_MAX; i++) {
		struct rtsp_cli *c = &g_cli[i];
		double lag_ms = 0, idle;

		if (!c->used)
			continue;
		conn++;
		if (!c->playing)
			continue;
		play++;
		sent += c->sent_pkts;
		/* lag: 游标距环最新的时间差(ms, 按 90kHz ts 差; 持环锁采样) */
		pthread_mutex_lock(&g_pkb_lock);
		if (c->cursor >= g_pkb_oldest_tag && c->cursor < g_pkb_next_tag) {
			uint32_t cur_ts = g_pkb[c->cursor % PKB_MAX].ts90;
			uint32_t new_ts =
				g_pkb[(g_pkb_next_tag - 1) % PKB_MAX].ts90;

			lag_ms = (double)(uint32_t)(new_ts - cur_ts) / 90.0;
		}
		pthread_mutex_unlock(&g_pkb_lock);
		idle = c->last_send_ms ?
		       (double)(now - c->last_send_ms) / 1000.0 : -1.0;
		printf("rtsp c%d: sent=%lu/%luB skip=%lu evict=%lu dead=%lu lag=%.0fms idle=%.1fs\n",
		       i, c->sent_pkts, c->sent_bytes, c->skip_frames,
		       c->evicted, c->sock_dead, lag_ms, idle);
	}
	pthread_mutex_unlock(&g_rtsp_lock);
	fds = rtsp_count_fds();
	printf("rtsp: conn=%d play=%d/%d sent=%lu epoch=%lu rej453=%lu fds=%d\n",
	       conn, play, g_max_clients, sent, ep, rej, fds);
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
 * 读一个完整 RTSP 请求. 返回 >0=请求长度, 0=超时(继续等), -1=断开/退出,
 * -2=半开超时(A3: deadline_ms 到期仍无 PLAY).
 * 200ms poll 让线程能看见 g_rtsp_stopping; 客户端发来的 interleaved RTCP
 * 包在缓冲里直接跳过, 只把 RTSP 请求交给上层.
 */
static int rtsp_read_req(int fd, char *buf, int *plen, int cap,
			 uint64_t deadline_ms)
{
	/* 不变量(评审): len 必须落在 [0, cap-1], 违反=更早已被破坏, 干净拒绝
	 * 而不是静默续写(多客户端后这条防御的价值更高)。 */
	if (cap < 8 || *plen < 0 || *plen > cap - 1) {
		fprintf(stderr,
			"[rtsp] req invariant broken plen=%d cap=%d\n",
			*plen, cap);
		return -1;
	}
	for (;;) {
		int len = *plen;
		char *p;

		/* A3: 半开超时(0=不限时), 200ms poll 粒度 */
		if (deadline_ms && now_ms_mono() >= deadline_ms)
			return -2;
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
			/* p+4 可能恰好是 buf+cap(请求头贴边): 越界写 0 会砸栈,
			 * 板上 gdb 实证 SIGSEGV@rtsp_hub.c:724。只清缓冲范围内的。 */
			if (p + 4 < buf + cap)
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

/* 每连接一线程: 信令阶段(OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN), PLAY 接受后
 * 同一线程进入发送循环(每客户端消费线程, cam_rtsp 模型)。
 * TEARDOWN 在发送循环期间不再读取(与 cam_rtsp 相同): 客户端断开由发送
 * FATAL(EPIPE/ECONNRESET)发现, 走同一收尾路径。 */
static void *rtsp_client_thread(void *arg)
{
	int slot = (int)(intptr_t)arg;
	struct rtsp_cli *c = &g_cli[slot];
	char req[RTSP_REQ_MAX];
	int reqlen = 0, cseq = 0;
	uint64_t dl = 0;	/* A3: >0 = 该时刻前必须 PLAY, 否则按半开关闭 */

	/* ---- 信令阶段 ---- */
	while (!g_rtsp_stopping && !g_hub_stopping) {
		char out[RTSP_SDP_MAX + 512];
		char sdp[RTSP_SDP_MAX];
		char b64s[512], b64p[512];
		char *p;
		int n = rtsp_read_req(c->fd, req, &reqlen, (int)sizeof(req), dl);

		if (n == -2) {
			printf("rtsp c%d: SETUP 后 10s 未 PLAY, 关闭半开连接\n",
			       slot);
			fflush(stdout);
			break;
		}
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
			if (rtsp_xwrite(c->fd, out, strlen(out)) < 0)
				break;
		} else if (!strncmp(req, "DESCRIBE", 8)) {
			uint8_t sps[sizeof(g_sps)], pps[sizeof(g_pps)];
			char plid[8];
			int sl, pl, waited;

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

			/* 参数集可能在首帧 IDR 才出现: 拷贝快照, 不持锁做 IO */
			pthread_mutex_lock(&g_nal_lock);
			sl = g_sps_len;
			pl = g_pps_len;
			if (sl > 0)
				memcpy(sps, g_sps, (size_t)sl);
			if (pl > 0)
				memcpy(pps, g_pps, (size_t)pl);
			pthread_mutex_unlock(&g_nal_lock);

			rtsp_profile_level(sps, sl, plid);
			if (sl > 0 && pl > 0) {
				/* H3: 不再检查"是不是当前唯一客户端",
				 * 每个连接都独立拿到 SDP */
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
			if (rtsp_xwrite(c->fd, out, strlen(out)) < 0)
				break;
		} else if (!strncmp(req, "SETUP", 5)) {
			char *tr = strstr(req, "interleaved=");
			int ch_rtp = 0, ch_rtcp = 1, rc;

			if (tr)
				sscanf(tr + 12, "%d-%d", &ch_rtp, &ch_rtcp);
			/* A2: 每客户端独立 ssrc/session(原来共用固定值) */
			snprintf(c->session, sizeof(c->session), "%08x",
				 (unsigned)(now_ms_mono() ^ (uintptr_t)c));
			c->ssrc = (uint32_t)(now_ms_mono() * 2654435761u) ^
				  (uint32_t)(uintptr_t)c;
			c->ch = ch_rtp;
			dl = now_ms_mono() + 10 * 1000;	/* A3: 10s 内必须 PLAY */
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
				 "Session: %s\r\n"
				 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%u\r\n\r\n",
				 cseq, c->session, ch_rtp, ch_rtcp,
				 (unsigned)c->ssrc);
			rc = rtsp_xwrite(c->fd, out, strlen(out));
			if (rc < 0)
				break;
		} else if (!strncmp(req, "PLAY", 4)) {
			int rc, ok = 0;
			uint64_t start_tag, ts0 = 0;

			/* 名额检查+占位必须原子, 否则并发 PLAY 会超卖
			 * (docs/38 §4.3: 拒绝只看 PLAY 数, DESCRIBE/SETUP
			 * 不占名额)。锁序 rtsp→pkb 与发送循环一致。 */
			pthread_mutex_lock(&g_rtsp_lock);
			pthread_mutex_lock(&g_pkb_lock);
			start_tag = pkb_latest_key();	/* §2: 从最新 IDR 起推 */
			if (start_tag < g_pkb_next_tag)
				ts0 = g_pkb[start_tag % PKB_MAX].ts90;
			if (count_playing_locked() >= g_max_clients) {
				g_rej453++;
			} else {
				c->playing = 1;
				c->cursor = start_tag;
				c->need_key = (start_tag >= g_pkb_next_tag);
				c->seq = 0;
				ok = 1;
			}
			pthread_mutex_unlock(&g_pkb_lock);
			pthread_mutex_unlock(&g_rtsp_lock);

			if (!ok) {
				/* B2: 453 语义 —— 不排队、不自动重试, 带
				 * Retry-After; 持续增长 = 客户端数超预算 */
				snprintf(out, sizeof(out),
					 "RTSP/1.0 453 Too Many Connections\r\nCSeq: %d\r\n"
					 "Retry-After: 5\r\n\r\n", cseq);
				rtsp_xwrite(c->fd, out, strlen(out));
				break;		/* 拒绝后关闭 */
			}
			/* 200 OK 必须完整先到, 之后才允许 RTP 上路 */
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n"
				 "Range: npt=0.000-\r\n"
				 "RTP-Info: url=rtsp://127.0.0.1:%d/live/trackID=0;seq=%u;rtptime=%u\r\n\r\n",
				 cseq, c->session, g_rtsp_port,
				 (unsigned)c->seq, (unsigned)ts0);
			rc = rtsp_xwrite(c->fd, out, strlen(out));
			if (rc < 0) {
				pthread_mutex_lock(&g_rtsp_lock);
				c->playing = 0;	/* OK 没发出去: 名额随连接释放 */
				pthread_mutex_unlock(&g_rtsp_lock);
				break;
			}
			printf("rtsp c%d: 客户端开始播放(从 tag %llu 起)\n",
			       slot, (unsigned long long)start_tag);
			fflush(stdout);
			break;			/* → 发送循环 */
		} else if (!strncmp(req, "TEARDOWN", 8)) {
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseq);
			rtsp_xwrite(c->fd, out, strlen(out));
			break;
		} else {
			/* GET_PARAMETER 及其它: 统一回 200, 保持会话不死 */
			snprintf(out, sizeof(out),
				 "RTSP/1.0 200 OK\r\nCSeq: %d\r\n\r\n", cseq);
			if (rtsp_xwrite(c->fd, out, strlen(out)) < 0)
				break;
		}
	}

	/* ---- 发送循环(仅 PLAY 已接受时进入; 本线程即该客户端的消费线程) ---- */
	while (c->playing) {
		struct txdiag dg = { TX_WHY_ERROR, 0, 0, 0, -1, -1 };
		uint64_t t, seq0;
		int flen, fkey, rc;
		uint32_t fts;

		if (g_rtsp_stopping || g_hub_stopping)
			break;
		pthread_mutex_lock(&g_rtsp_lock);
		pthread_mutex_lock(&g_pkb_lock);
		/* 防御: 清环/恢复后 cursor 可能越过队尾, 收敛回去 */
		if (c->cursor > g_pkb_next_tag)
			c->cursor = g_pkb_next_tag;
		/* 被动逐出(A1): cursor 落到环外 = 生产者覆盖了未发包 */
		if (g_pkb_oldest_tag < g_pkb_next_tag &&
		    c->cursor < g_pkb_oldest_tag) {
			c->evicted +=
				(unsigned long)(g_pkb_oldest_tag - c->cursor);
			c->cursor = g_pkb_oldest_tag;
			c->need_key = 1;	/* 中间帧不可解, 等 IDR */
		}
		if (c->need_key) {
			uint64_t t2 = pkb_find_key_from(c->cursor);

			if (t2 < g_pkb_next_tag) {
				c->skip_frames +=
					(unsigned long)(t2 - c->cursor);
				c->cursor = t2;
				c->need_key = 0;
			} else {
				c->cursor = g_pkb_next_tag;
			}
		}
		if (c->cursor >= g_pkb_next_tag) {
			/* 已追平: 先放 rtsp 锁再等环(等待时不占两把锁) */
			pthread_mutex_unlock(&g_rtsp_lock);
			pthread_cond_wait(&g_pkb_cond, &g_pkb_lock);
			pthread_mutex_unlock(&g_pkb_lock);
			continue;
		}
		/* A1 不变量: 锁内 memcpy 到私有缓冲, 出锁后只碰私有缓冲 */
		t = c->cursor;
		{
			struct pktbuf *p = &g_pkb[t % PKB_MAX];

			flen = p->len;
			fkey = p->key;
			fts = p->ts90;
			if (flen > 4 * 1024 * 1024) {
				/* 防御: 异常大帧按被动丢处理, 等 IDR 重新同步 */
				c->evicted++;
				c->need_key = 1;
				c->cursor = t + 1;
				pthread_mutex_unlock(&g_pkb_lock);
				pthread_mutex_unlock(&g_rtsp_lock);
				continue;
			}
			if (flen > c->scap) {
				uint8_t *nb = realloc(c->sbuf, (size_t)flen);

				if (!nb) {
					c->evicted++;
					c->need_key = 1;
					c->cursor = t + 1;
					pthread_mutex_unlock(&g_pkb_lock);
					pthread_mutex_unlock(&g_rtsp_lock);
					continue;
				}
				c->sbuf = nb;
				c->scap = flen;
			}
			memcpy(c->sbuf, p->data, (size_t)flen);
		}
		pthread_mutex_unlock(&g_pkb_lock);
		pthread_mutex_unlock(&g_rtsp_lock);

		/* 发送在全部锁外; 整包语义(P10)不变: 帧内失败=连接作废 */
		seq0 = c->seq;
		rc = rtsp_send_frame(c, c->sbuf, flen, fkey, fts, &dg);
		if (rc != RTSP_TX_OK) {
			pthread_mutex_lock(&g_rtsp_lock);
			c->sock_dead++;
			if (dg.why == TX_WHY_ERROR &&
			    (dg.err == EPIPE || dg.err == ECONNRESET))
				g_rtsp_disc++;
			pthread_mutex_unlock(&g_rtsp_lock);
			printf("rtsp c%d: 发送判死(why=%s errno=%d), 断开\n",
			       slot, tx_why_name(dg.why), dg.err);
			fflush(stdout);
			break;
		}
		pthread_mutex_lock(&g_rtsp_lock);
		c->cursor = t + 1;
		/* sent_pkts 用 seq 差精确计数(每个 RTP 包恰好 ++ 一次) */
		c->sent_pkts +=
			(unsigned long)(uint16_t)(c->seq - (uint16_t)seq0);
		c->sent_bytes += (unsigned long)flen;
		c->last_send_ms = now_ms_mono();
		pthread_mutex_unlock(&g_rtsp_lock);
	}

	/* ---- 收尾: 标记待回收, fd 归本线程关; accept 线程 join 后释放槽 ---- */
	{
		int fd = c->fd;

		pthread_mutex_lock(&g_rtsp_lock);
		c->playing = 0;
		c->fd = -1;	/* 防 rtsp_stop 对已关闭(可能复用)的 fd shutdown */
		c->th_done = 1;
		pthread_mutex_unlock(&g_rtsp_lock);
		shutdown(fd, SHUT_RDWR);
		close(fd);
	}
	printf("rtsp c%d: 连接关闭\n", slot);
	fflush(stdout);
	return NULL;
}

/* 回收已退出线程的槽: join 后才释放(槽复用以 join 完成为前提, 防栈泄漏) */
static void rtsp_reap_done(void)
{
	int i;

	for (i = 0; i < RTSP_CLI_MAX; i++) {
		pthread_t tid;
		int need;

		pthread_mutex_lock(&g_rtsp_lock);
		need = g_cli[i].used && g_cli[i].th_done && !g_cli[i].joined;
		if (need) {
			tid = g_cli[i].tid;
			g_cli[i].joined = 1;
		}
		pthread_mutex_unlock(&g_rtsp_lock);
		if (!need)
			continue;
		pthread_join(tid, NULL);
		pthread_mutex_lock(&g_rtsp_lock);
		g_cli[i].used = 0;
		if (g_cli[i].sbuf) {
			free(g_cli[i].sbuf);
			g_cli[i].sbuf = NULL;
			g_cli[i].scap = 0;
		}
		pthread_mutex_unlock(&g_rtsp_lock);
	}
}

static void *rtsp_accept_thread(void *arg)
{
	(void)arg;
	while (!g_rtsp_stopping && !g_hub_stopping) {
		struct pollfd pf;
		struct rtsp_cli *c;
		int pr, fd, one = 1, slot = -1, i;

		rtsp_reap_done();
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
		for (i = 0; i < RTSP_CLI_MAX; i++)
			if (!g_cli[i].used) {
				slot = i;
				break;
			}
		if (slot < 0) {
			/* 槽满(≥ max_clients+2 个半开): 连握手都做不了, 直接
			 * close; 计入 rej453(容量指标, docs/38 §4.3) */
			g_rej453++;
			pthread_mutex_unlock(&g_rtsp_lock);
			close(fd);
			printf("rtsp: 槽满, close\n");
			fflush(stdout);
			continue;
		}
		c = &g_cli[slot];
		memset(c, 0, sizeof(*c));
		c->used = 1;
		c->fd = fd;
		pthread_mutex_unlock(&g_rtsp_lock);

		if (pthread_create(&c->tid, NULL, rtsp_client_thread,
				   (void *)(intptr_t)slot) != 0) {
			pthread_mutex_lock(&g_rtsp_lock);
			c->used = 0;
			pthread_mutex_unlock(&g_rtsp_lock);
			close(fd);
			continue;
		}
		printf("rtsp: 客户端接入(槽 %d)\n", slot);
		fflush(stdout);
	}
	return NULL;
}

int rtsp_start(int port)
{
	g_rtsp_port = port;	/* H1 评审阻断一: hub 发送门/SDP/pkb_push 扫描门都读它 */
	/* H1 遗留修复: 提炼时把 fopen 丢了 —— 830bf53 中 g_rtp_trace 无任何
	 * 赋值点, --rtp-trace 自 H1 起路径只被存储、从未打开(死功能) */
	if (g_rtp_trace_path && !g_rtp_trace) {
		g_rtp_trace = fopen(g_rtp_trace_path, "w");
		if (g_rtp_trace)
			setvbuf(g_rtp_trace, g_rtp_trace_buf, _IOFBF,
				sizeof(g_rtp_trace_buf));
		else
			fprintf(stderr, ">>> --rtp-trace 打开失败: %s\n",
				g_rtp_trace_path);
	}
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
	printf(">>> RTSP 监听 :%d (多客户端, 上限 %d)\n", port, g_max_clients);
	fflush(stdout);
	return 0;
}

/* 退出: 让 accept/客户端线程退并逐槽 join, 再关监听 fd */
void rtsp_stop(void)
{
	int i;

	if (!g_rtsp_accept_started && g_rtsp_lfd < 0)
		return;
	g_rtsp_stopping = 1;
	if (g_rtsp_accept_started) {
		pthread_join(g_rtsp_accept_tid, NULL);
		g_rtsp_accept_started = 0;
	}
	/* 唤醒等环的客户端线程 + 解除 send/poll 阻塞 */
	pkb_set_stop();
	pthread_mutex_lock(&g_rtsp_lock);
	for (i = 0; i < RTSP_CLI_MAX; i++)
		if (g_cli[i].used && g_cli[i].fd >= 0)
			shutdown(g_cli[i].fd, SHUT_RDWR);
	pthread_mutex_unlock(&g_rtsp_lock);
	/* 逐槽 join(fd=-1 已由线程自关; 线程在 stopping/关闭的 fd 上会尽快退出) */
	for (i = 0; i < RTSP_CLI_MAX; i++) {
		pthread_t tid;
		int need;

		pthread_mutex_lock(&g_rtsp_lock);
		need = g_cli[i].used && !g_cli[i].joined;
		if (need) {
			tid = g_cli[i].tid;
			g_cli[i].joined = 1;
		}
		pthread_mutex_unlock(&g_rtsp_lock);
		if (!need)
			continue;
		pthread_join(tid, NULL);
		pthread_mutex_lock(&g_rtsp_lock);
		g_cli[i].used = 0;
		if (g_cli[i].sbuf) {
			free(g_cli[i].sbuf);
			g_cli[i].sbuf = NULL;
			g_cli[i].scap = 0;
		}
		pthread_mutex_unlock(&g_rtsp_lock);
	}
	if (g_rtsp_lfd >= 0) {
		close(g_rtsp_lfd);
		g_rtsp_lfd = -1;
	}
	rtp_trace_close();	/* trace 收尾落盘(退出路径, 注释声称的兜底) */
}

/* ---------------- RTSP 分发环(编码线程 -> N 个客户端线程) ----------------
 *
 *   - 编码线程对每个编码包 malloc+memcpy 一份入环, 双上限(包数 depth /
 *     字节 ring_mb, B3)任一超限丢最旧, 绝不等待消费者; 慢客户端只丢它
 *     自己的帧(游标被逐出后等 IDR), 不反压采集/编码/事件录像;
 *   - H3 起没有独立 sender 线程: 每个客户端线程按私有游标消费(A1 锁内
 *     memcpy), tag 连续 [oldest, next), 物理槽 = tag % PKB_MAX;
 *   - 停止只认 g_pkb_stop_local(pkb_set_stop 锁内置位+广播);
 *   - 录像环/OSD/检测路径不经过这里。
 */

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

void rtsp_hub_set_ring_depth(int n)
{
	if (n < 1)
		n = 1;
	if (n > PKB_MAX)
		n = PKB_MAX;	/* PKB_MAX=64: 运行期调环深必须 clamp, 否则 --rtsp-ring 100 越界写 */
	g_pkb_depth = n;
}
void rtsp_hub_set_trace_path(const char *path)	{ g_rtp_trace_path = path; }

void rtsp_hub_set_max_clients(int n)
{
	if (n < 1)
		n = 1;
	if (n > 8)
		n = 8;	/* 槽表 10 = 上限 8 + 2 半开余量 */
	g_max_clients = n;
}

void rtsp_hub_set_deadline(int s)
{
	if (s < 1)
		s = 1;
	if (s > 120)
		s = 120;	/* 硬上限是防堆积兜底, 不收 10 分钟级停读 */
	g_deadline_s = s;
}

void rtsp_hub_set_ts_offset(uint32_t off)
{
	g_ts_off = off;
}

void rtsp_hub_set_ring_mb(int mb)
{
	if (mb < 1)
		mb = 1;
	if (mb > 64)
		mb = 64;
	g_pkb_mb = mb;
}

/* 编码线程: 拷贝一份入环, 双上限(包数/字节)任一超限丢最旧。计数与队列
 * 同一把锁, 天然原子; free 一律锁外(victims 暂存, 单次最多 depth 个)。 */
void pkb_push(const unsigned char *src, int len, unsigned seq, int key,
	      double ts_ms)
{
	struct pktbuf *p;
	unsigned char *copy;
	unsigned char *victims[PKB_MAX];
	unsigned long ev_bytes = 0;
	uint64_t tag;
	int i, nv = 0;

	if (!g_pkb_started || len <= 0)
		return;
	copy = malloc((size_t)len);
	if (!copy) {
		pthread_mutex_lock(&g_pkb_lock);
		g_pkb_drop++;
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
	/* B3 双上限逐出: 字节优先构成内存上界; 单帧超上限时清空后仍入环。
	 * 丢最旧即客户端游标可能落到环外 —— 各端在下一次消费时按 A1 判定
	 * evicted 并等 IDR, 不再需要 H1 的全局 gap 标志。 */
	while (g_pkb_oldest_tag < g_pkb_next_tag &&
	       ((int)(g_pkb_next_tag - g_pkb_oldest_tag) >= g_pkb_depth ||
		g_pkb_bytes + (unsigned long)len >
		(unsigned long)g_pkb_mb << 20)) {
		p = &g_pkb[g_pkb_oldest_tag % PKB_MAX];

		if (nv < PKB_MAX)
			victims[nv++] = p->data;
		p->data = NULL;
		ev_bytes += (unsigned long)p->len;
		g_pkb_oldest_tag++;
		g_pkb_drop++;
	}
	tag = g_pkb_next_tag;
	p = &g_pkb[tag % PKB_MAX];
	p->data = copy;
	p->len = len;
	p->seq = seq;
	p->key = (unsigned char)(key ? 1 : 0);
	p->ts90 = (uint32_t)(ts_ms * 90.0) + g_ts_off;	/* g_ts_off=注入回绕用, 0=无 */
	p->tag = tag;
	g_pkb_next_tag = tag + 1;
	g_pkb_bytes += (unsigned long)len - ev_bytes;
	g_pkb_in++;
	pthread_cond_broadcast(&g_pkb_cond);	/* 多客户端: 全部唤醒 */
	pthread_mutex_unlock(&g_pkb_lock);

	for (i = 0; i < nv; i++)
		free(victims[i]);	/* 锁外释放: 不占着环锁调用 allocator */
}

/* 把 rtsp_hub_set_stop() 注入的停止状态在锁内同步成 sender 的停止条件 */
void pkb_set_stop(void)
{
	pthread_mutex_lock(&g_pkb_lock);
	g_pkb_stop_local = 1;
	pthread_cond_broadcast(&g_pkb_cond);
	pthread_mutex_unlock(&g_pkb_lock);
}

void rtsp_ring_report(void)
{
	unsigned long in, drop, bytes;
	uint64_t oldest, next;

	pthread_mutex_lock(&g_pkb_lock);
	in = g_pkb_in;
	drop = g_pkb_drop;
	bytes = g_pkb_bytes;
	oldest = g_pkb_oldest_tag;
	next = g_pkb_next_tag;
	pthread_mutex_unlock(&g_pkb_lock);
	printf("rtsp-ring: in=%lu evict=%lu depth=%d bytes=%.2f/%dMB tags=[%llu,%llu)\n",
	       in, drop, g_pkb_depth, bytes / 1048576.0, g_pkb_mb,
	       (unsigned long long)oldest, (unsigned long long)next);
	fflush(stdout);		/* 后台重定向到日志时也要实时可见 */
}

int pkb_start(int depth)
{
	/* depth>0 覆盖 --rtsp-ring 默认; 0=沿用现值(默认 30 或 set_ring_depth)。
	 * clamp 必须对 g_pkb_depth 自身做, 0 路径不得冲掉已设置的值。 */
	if (depth > 0)
		g_pkb_depth = depth;
	if (g_pkb_depth < 1)
		g_pkb_depth = 1;
	if (g_pkb_depth > PKB_MAX)
		g_pkb_depth = PKB_MAX;
	/* H3: 不再有独立 sender 线程 —— 每个客户端线程自己消费环 */
	g_pkb_started = 1;
	return 0;
}

/* 退出: 释放环内残留(线程回收由 rtsp_stop 负责) */
void pkb_stop(void)
{
	int i;

	if (!g_pkb_started)
		return;
	pkb_set_stop();
	pthread_mutex_lock(&g_pkb_lock);
	for (i = 0; i < PKB_MAX; i++) {
		free(g_pkb[i].data);
		g_pkb[i].data = NULL;
	}
	g_pkb_bytes = 0;
	g_pkb_oldest_tag = g_pkb_next_tag;
	pthread_mutex_unlock(&g_pkb_lock);
	g_pkb_started = 0;
}