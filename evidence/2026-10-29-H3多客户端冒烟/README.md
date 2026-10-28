# 2026-10-29 H3 多客户端板上冒烟(docs/38 V 矩阵的 lite 版)

## 绑定

- 代码 commit: 7c07230(H3 实现);源码 md5: rtsp_hub.c=460ca47b, rtsp_hub.h=8b4add28,
  evrec.c=75fcec59(PC 与板端逐一核对一致)
- binary md5 = 41806b05(/tmp/h3src/evrec_h3, 板端 aarch64 gcc 9.4.0, -O2 -Wall)
- rtsp_hub.c 编译零警告;evrec.c 仅剩 H1/S2 时代既有 4 条警告(unused k/max_event_sec/
  ring_clear/format-truncation),与 H3 无关

## ★别名勘误(本次差点白干)

Windows ~/.ssh/config: `rk3568` = 192.168.239.128 是 **x86_64 SDK 虚拟机**(uname 实证),
不是板子;真板 = `rkboard`(192.168.10.2, root, aarch64)。误连 VM 期间只做了 mkdir/
scp/dpkg -x 解包(/tmp/h3src、/tmp/mppx),无系统改动。板子全程没有重启——H1 服务器
(2fb0376b1e)一直在跑,直到本次被 SIGTERM 优雅替换。

## 冒烟结果(全过)

| # | 场景 | 结果 |
|---|---|---|
| 1 | 单客户端 ffprobe | `stream,h264,1920,1080` ✓ |
| 2 | 2 并发 ffmpeg 8s 拉流 | 双双 exit 0;服务端 `conn=2 play=2/4`,c0/c1 各自统计(sent≈1700包/2.27MB 一致,lag=0,evict=0)✓ |
| 3 | 453(--rtsp-clients 1 时第 2 客户端) | ffprobe 报 `method PLAY failed: 453 Too Many Connections`;既有 12s 拉流全程无感 ✓ |
| 4 | 断开检测 | ECONNRESET(errno=104)→ 发送判死+干净关闭 ✓ |
| 5 | 半开超时 / epoch / B3 字节逐出压测 | **未测**(进 H5 板测矩阵) |

统计行新格式: `rtsp: conn= play=N/M sent= epoch= rej453= fds=` + 每客户端
`rtsp cN: sent=包/字节 skip= evict= dead= lag= idle=`(B5 口径: 包不是帧,
被动逐出 evict 与主动跳帧 skip 分开)。

## 文件
- `h3a-server-clients1-453.log` — --rtsp-clients 1 会话(含 453 拒绝时段)
- `h3b-server-2clients.log` — 默认 4 客户端上限的双并发会话
