# 2026-10-29 V7 双客户端 2h soak(docs/38 §5 V7;24h soak 的前置档)

## 绑定

- 代码 = feature/rtsp-hub-h1 @ 09b66eb(源码 md5: hub.c=460ca47b / hub.h=8b4add28 /
  evrec.c=2e6bda09,PC=板端);binary = /tmp/h3src/evrec_h3 md5 **8cbc13aa**(aarch64 gcc 9.4)
- 配置 = 1080p / kbps 4000 / ring 30 包+8MB / --rtsp 8554(默认 4 客户端上限),无 --detect
- 服务器 PID 192805,05:26~07:28(板载时钟),共 **2h02m**;客户端 = PC 两路
  `ffmpeg -t 7260 -f null`(7260s 流时间)

## 判定(docs/38 §5 V7: 无 fd 泄漏、RSS 有界)

| 判据 | 结果 |
|---|---|
| 客户端 | A/B 双双 exit 0;stderr 仅 3 条等值 DTS 警告(dts 22551>=22551,既往实测同型=已知噪音,两客户端数值完全相同=游标零分叉又一证) |
| 发送 | c0/c1 终态 **sent=1382716 包 / 1,837,948,734 B,逐字节一致**(190 RTP 包/s ≈ 15.9 帧/s × 12 包/帧 ≈ 253 KB/s,与环 in 速率自洽);skip=evict=dead=0,lag=0ms 全程 |
| fd 无泄漏 | 采样 121 针:fds=16 恒定(基线 14+2 客户端),断开后回 14;服务端日志 fds 分布 14×372/15×1/16×7260,无增长 ✓ |
| RSS 有界 | **49.5~54.1MB**:包络 +4.6MB/2h 轻度上移但非单调(06:11 回落 1MB);每包泄漏已排除——2h 约 137 万次包分配若带真泄漏应为百 MB 级,实测 4.6MB≈碎片/arena 量级。**注记:上行趋势未收敛,24h soak 必须复核;若延续再定位**(候选:尺寸类碎片/线程栈触碰) |
| 稳定性 | invariant broken=0,rej453=0,epoch=0(无故障注入),无 crash,进程存活,SIGTERM 优雅退出 ✓ |

结论:**V7 PASS**(RSS 注记如上)。24h soak 是 tag 前最后一关。

## 文件
- `v7-monitor.log` — 板上采样器每 60s 一针(时间戳/rss kB/fds/threads),md5 双端一致
- `v7-server.log` — 服务端全量日志(ring 收支 + 每秒聚合 + 每客户端行)
- 采样器脚本随本提交入库:`tools/v7mon.sh`
