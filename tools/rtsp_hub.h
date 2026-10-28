/* rtsp_hub.h —— evrec 使用的 RTSP 服务端+投递环接口(H1 提炼, 签名与原 static 一致) */
#ifndef RTSP_HUB_H
#define RTSP_HUB_H

#include <stdint.h>

void rtsp_hub_set_stop(void);
void rtsp_hub_notify_recover(void);
void rtsp_hub_poll_reports(double now_ms_rel);
void rtsp_hub_get_ps(uint8_t *sps, int sps_cap, uint8_t *pps, int pps_cap,
		     int *sl, int *pl);
void rtsp_hub_set_ring_depth(int n);
void rtsp_hub_set_trace_path(const char *path);		/* evrec 信号路径注入停止(替代直读 g_stop) */

void rtsp_scan_nals(const uint8_t *d, int n);		/* 缓存 SPS/PPS */
int  rtsp_has_nal(const uint8_t *d, int n, int type);
int  rtsp_start(int port);
void rtsp_stop(void);
void pkb_push(const unsigned char *src, int len, unsigned seq,
	      unsigned char key, uint32_t ts90);
int  pkb_start(int depth);
void pkb_set_stop(void);
void pkb_stop(void);
void rtsp_ring_report(void);
void rtsp_stats_report(void);

#endif
