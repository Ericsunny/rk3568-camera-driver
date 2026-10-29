# 2026-10-29 H5 板测矩阵(docs/38 §5,V7 除外)

## 绑定

- 代码 = feature/rtsp-hub-h1 @ e4fd3ce 起测;测试中途新增 5273d8d(删 [evw] ringpush-call
  热路径 debug 残留,见下"过程发现"),最终窗口跑在 5273d8d 源码上
- 源码 md5: hub.c=460ca47b / hub.h=8b4add28 / evrec.c=2e6bda09(5273d8d 后, 双端一致)
- binary = /tmp/h3src/evrec_h3, md5 **8cbc13aa**(5273d8d);早期窗口用 41806b05(e4fd3ce,
  与 10-28 冒烟相同=可复现编译)
- 板子经历断电重启(up 2 min 起), /tmp 全清后重部署, 头文件/库在 /usr 不受影响

## Phase 1(默认 4 客户端上限, ring 30 包/8MB, binary 41806b05)

| # | 场景 | 结果 |
|---|---|---|
| A3 | OPTIONS+SETUP 后不 PLAY 保持 14s | 服务端 10s 到点关闭半开连接; **同时段 ffprobe 正常起播** = 半开不占播放名额 ✓ |
| V5 | ffprobe 连断 ×10 | 10/10 拿到 h264; fds 回基线 14; invariant broken=0; 无 crash ✓ |
| V2 | A 拉 15s, +5s B 加入 | B 立即解出 h264; A 全程 exit 0; c0 从 tag 2130 / c1 从 tag 2190 起播(各自最新 IDR, 游标独立)✓ |
| V3a | 裸 socket 停读 2s 后恢复 | 存活续收(1.26MB→2.98MB), 未判死 ✓ |
| V3b | 裸 socket 停读 30s | 服务端判死(why=stall errno=11)✓ |
| V3 隔离 | 停读 40s 期间健康 ffmpeg 拉 12s | 健康客户端 exit 0; 慢客户端 stall 判死 ✓ |
| V1 | 双 ffmpeg 并发 660s | 双双 exit 0 零 stderr; 流时间≈墙钟(采集无减速); conn=2 全程稳定; **c0/c1 发送逐字节一致**(14675包/19467627B@4.5min); fds 全程仅 14/15/16 三态; rej453=0; 进程存活 ✓ |

注: V3 判死后客户端仍 recv 到 FIN 前已入内核缓冲的数据=正常 TCP 行为, 非服务端续推。
V3 判死实测路径 = 单包 POLLOUT 等满 1s×10(§4.4 有界等待), 非 --rtsp-deadline-s
(硬上限属 H4 范围, 未实现)。

## Phase 2(B3 字节上限, ring 64 包+2MB)

- 1080p 静态场景 64 包仅 ~1MB, 包数上限先 binding(字节上限不触发——正常);
- 换 4K(kbps 8000, 大 IDR): bytes 峰值 **1.86MB 未破 2.00MB**, 高字节时持包数被
  压到 55(<64)= **字节优先逐出实证** ✓(phase2-b3-4k.log)

## Phase 3/4+5(V6 事件并存 + B4 epoch, --detect + yolov8n, binary 8cbc13aa)

| # | 场景 | 结果 |
|---|---|---|
| B4 | --fault-sim dqbuf:300, 客户端在故障前接入 | 故障→恢复 1/2 成功→**epoch=1**; 客户端 sent 289→23683 贯穿故障+恢复, 150s 零 >1s 断流 ✓ |
| V6 | --detect 推流同时事件录像 | 3 个事件全部 close+rename+JSON 齐全(close_reason=target_left×2/sigterm×1, write_error=false, coalesced=5/2/31); 推流与事件互不影响 ✓ |
| fds | --detect 基线 | 30→32 有界波动(NPU/RGA/dma-buf), 非泄漏; 无 --detect 时基线 14-16 |

事件 JSON 抽查(v6f_*-0*.json): 八字段+segment_index/event_id/coalesced 全齐,
duration_ms 与 first/last_ts_ms 自洽。

## 过程发现(已修)

- **[evw] ringpush-call fp=%p 热路径 debug 残留**(S2 开发期探针, 事件活跃时每包一行
  stderr)→ 5273d8d 删除, claims 工具验证 pass=1/fail=0, HEAD 中计数 0;
- **pgrep -f 会匹配启动器 bash 命令行**(命令行里含同样字符串)→ kill 不到真进程,
  新实例因 /dev/video5 被占直接退出; 正解=`pgrep -x evrec_h3`(comm 8 字符不受
  15 字符截断影响)按 PID kill;
- 静态背景检测分数 0.57~0.83 反复触发→事件被 S1 延长语义一直续命(173s/37MB),
  即已知的 --det-score 灵敏度待调优项, 非新缺陷。

## 未跑(如实)

- **V7(2h)**: docs/38 要求 V1 过后单独窗口; H6(cam_rtsp 退役)被 V7 阻塞中;
- --rtsp-deadline-s 硬上限(H4 范围);
- 4 客户端并发只验到 2(设备单客户端上限已由昨日矩阵覆盖, 453 判据已验)。

## 文件
- `phase1.log` — A3/V5/V2/V3/V1 全部服务端日志
- `phase2-b3.log` / `phase2-b3-4k.log` — B3 字节压测(1080p 对照 / 4K 实证)
- `phase34-v6-epoch.log` — 第一轮 detect 窗口(epoch 首证 + 长事件)
- `phase5-fault-midstream.log` — 中途故障+贯穿窗口(B4 客户端侧实证)
- `v6f_*-0{1,2,3}.json` — 三个事件的收口 JSON
