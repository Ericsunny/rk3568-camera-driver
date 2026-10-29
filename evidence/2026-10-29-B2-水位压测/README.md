# 2026-10-29 B2 磁盘水位压测(docs/37 S4 评审项 B2)

## 绑定

- binary = /tmp/h3src/evrec_h4 md5 **13f2113b**(= H4 提交 0e931aa 的板端编译;
  B2 测的是 S4 代码, 该代码自 PR #6 起未动)
- 配置 = 1080p --detect(yolov8n)+ **--disk-min-mb 6000**(板空闲 7272MB,
  恢复带 = 6000+1000 滞回);事件由镜头内常态在座者自动触发(阈值 0.45; 当时误记为"静态背景", 抓帧核实为真人在座, 见 H5 README 更正)

## 压测时序与结果(全过)

| 步骤 | 操作 | 观测 |
|---|---|---|
| 1 | 起服务器, 等事件 1 自动开 | `open event_id=1`, -01.part 在写 ✓ |
| 2 | dd 1500MB filler → 空闲 5692MB(< 6000) | ≤10s 缓存窗口内:`磁盘低水位(5691MB < 6000MB)` + `事件 1 完成(1 段, disk_low)`;.part 即刻 rename 为 .h264+.json ✓ |
| 3 | JSON 核验 | `close_reason=disk_low, event_id=1, write_error=false, disk_free_mb=5688, duration=419.8s` ✓ |
| 4 | 阻塞期观察 25s | 检测触发继续计 9 次(统计照走), 但 open 恒 1、.part 为 0 —— 触发闸生效 ✓ |
| 5 | rm filler → 空闲 7189MB | `磁盘回升(7188MB >= 7000MB), 恢复录像` —— 滞回带(min+1000)精确命中 ✓ |
| 6 | 恢复后首个触发 | `open event_id=2` 恰一次(无双计);SIGTERM 收口 `close_reason=sigterm, event_id=2, write_error=false` ✓ |

**核心断言**:evrec.c:1133 注释所指的历史 bug("disk_low 分支先 finish 又掉进
else 再 finish: g_events 双计")已修——水位收口单点完成, event_id 连续无跳变
无重复; 水位计入打开中的事件体积(收口时事件已写 86.5MB)。

## 文件
- `b2-server.log` — 全程服务端日志(md5 双端一致 31741b00)
- `b2_*-01.json` / `b2_*-02.json` — 两个事件的收口 JSON
