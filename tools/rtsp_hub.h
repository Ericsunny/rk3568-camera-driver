/* rtsp_hub.h —— evrec 使用的 RTSP 服务端+分发环接口(H3 多客户端) */
#ifndef RTSP_HUB_H
#define RTSP_HUB_H

#include <stdint.h>

void rtsp_hub_set_stop(void);		/* evrec 信号路径注入停止(替代直读 g_stop) */
void rtsp_hub_notify_recover(void);	/* B4: 链路恢复(epoch++ + 清环) */
void rtsp_hub_poll_reports(double now_ms_rel);
void rtsp_hub_get_ps(uint8_t *sps, int sps_cap, uint8_t *pps, int pps_cap,
		     int *sl, int *pl);
void rtsp_hub_set_ring_depth(int n);	/* 包数上限, clamp [1,64] */
void rtsp_hub_set_ring_mb(int mb);	/* B3: 字节上限(MB), clamp [1,64] */
void rtsp_hub_set_max_clients(int n);	/* --rtsp-clients, clamp [1,8] */
void rtsp_hub_set_deadline(int s);	/* --rtsp-deadline-s 硬上限(§4.4), clamp [1,120] */
void rtsp_hub_set_ts_offset(uint32_t off);	/* --ts-offset: RTP ts 注入偏移(回绕测试) */
void rtsp_hub_set_trace_path(const char *path);

void rtsp_scan_nals(const uint8_t *d, int n);		/* 缓存 SPS/PPS */
int  rtsp_has_nal(const uint8_t *d, int n, int type);
int  rtsp_start(int port);
void rtsp_stop(void);
/* 注意: 原型与 rtsp_hub.c 的定义一致(int key, double ts_ms)。
 * H1 的头文件曾误写为 (unsigned char, uint32_t), 与定义跨编译单元不匹配
 * (AArch64 上整数/浮点参数走不同寄存器组), 本次一并修正。 */
void pkb_push(const unsigned char *src, int len, unsigned seq,
	      int key, double ts_ms);
int  pkb_start(int depth);
void pkb_set_stop(void);
void pkb_stop(void);
void rtsp_ring_report(void);
void rtsp_stats_report(void);

#endif
