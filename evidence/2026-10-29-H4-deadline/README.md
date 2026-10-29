# 2026-10-29 H4 硬上限 --rtsp-deadline-s(docs/38 §4.4/§6 H4)

## 绑定

- 源码 md5(PC=板端): rtsp_hub.c=f6d32a9e / rtsp_hub.h=ed796c86 / evrec.c=aac8b716
- binary = /tmp/h3src/evrec_h4 md5 **13f2113b**(aarch64 gcc 9.4, -O2 -Wall,
  hub 零新增警告; evrec 仅剩既有 6 条)

## 实现(H4 前=单包 POLLOUT 等 1s×10 即死)

判死条件从"单包 1s 等满"改为"**连续 deadline 秒(默认 5)零进展**":自首次
EAGAIN 起锚点计时,期间任何字节推进(短写/后续包成功)清零重来;到点 FATAL
(why=stall errno=11)。整帧语义(P10)不变——卡住的帧要么等恢复发完,要么随
连接死,绝无半帧存活。停读期环照常滚,该客户端被逐出后恢复走 A1(等 IDR,
evict 计数,日志中 evict>0 共 150 行可见)。

## 板测(binary 13f2113b, --rtsp-deadline-s 5)

| # | 场景 | 结果 |
|---|---|---|
| V3c | 停读 3.5s ×2(同连接, 宽缓冲) | 两段均存活续收(0.85→2.84→4.80MB)——旧代码单包 1s 判死会杀掉第二段 ✓ |
| V3c' | SO_RCVBUF=64KB 压吸收 + 停读 3.5s | 存活(deadline 宽限内恢复, 真·锚点路径)✓ |
| V3d' | SO_RCVBUF=64KB + 真停读 12s | **t≈15s 判死**(3s 正常+~2s 内核吸收+5s deadline),服务端日志 `why=stall errno=11` ✓ |
| V3 隔离 | V3d' 同型停读期间健康 ffmpeg 10s 拉流 | exit 0 ✓ |
| 收尾 | 全部测试后 ffprobe | h264 正常;SIGTERM 优雅退出;日志 md5 双端一致(7c0f68d0) |

**测试方法教训(如实)**:第一版 V3d 忘写 `time.sleep()`(客户端根本没停读,
sent 持续增长即证据);第二版吸收容量误算——Windows 客户端接收窗口自动扩到
数 MB,253KB/s 的流 ~25s 才灌满,EAGAIN 根本不出现。最终用 SO_RCVBUF=64KB
把总吸收压到 ~0.5MB 才真正压到锚点时钟。

## 文件
- `h4-server.log` — 服务端全量日志(V3c/V3c'/V3d' 三轮 + 各会话统计行)
