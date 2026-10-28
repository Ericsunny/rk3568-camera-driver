# 2026-10-28 事件治理 S1+S2 板上验证

分支 `feature/event-governance`(@8034d9c),设计见 `docs/37-事件治理设计.md`。

## S1(last_detect_ms 延长判定)+ S2(流式写入)实现摘要

- `g_last_detect_ms`:检测命中即刷新(CLOCK_MONOTONIC,与主循环 ts 同源);
- 事件结束:`now - g_last_detect_ms > post_sec`——不再锚定首次触发时刻;
- 流式写入:`evw_open`(开 `.part`,pre-roll 从 ring 的 IDR 起写)→ `ring_push` 单点挂
  `evw_write`(seq 过滤,4MB 用户态缓冲批量写,写失败只置标志不阻塞采集)→
  `evw_close`(flush+close+rename+JSON);
- 事件收尾移除 `ring_clear`(ring 照滚供下一个 pre);streak 降级为触发门限语义。

## 板上 150 s 功能验证(4K+检测)

| 项 | 结果 |
|---|---|
| 触发次数 | **2**(修复前同一场景 433 次/4 min——`g_trigger` 未消费的 bug) |
| 事件数 | **1 个**(`gov_test-01`,目标持续在场期间未拆分) |
| `close_reason` | `target_left` |
| bytes | 19,407,398(≈19.4 MB,约 100 s 事件) |
| `write_error` | false |
| 采集 fps | 7.5(基线 7.3,无下降) |
| dmesg | `timeout/Unable to handle` 计数 0 |
| 文件可解码性 | ffprobe 识别 h264/3840×2160,`nb_read_frames=36`(抽帧计数方式限制,非全量) |

## 过程中抓到并修复的三个缺陷(均为板上实测暴露)

1. `evw_open` 成功路径未消费 `g_trigger` → 目标在场时每帧重开文件(433 次);
2. `g_out_base` 初始化从未插入(patch 断言中断)→ 事件文件落 `/root/-06.part`;
3. `-o` 分支插行方式破坏 else-if 链(编译期 `else without if`)。

## 遗留(S3 处理)

- JSON `duration_ms=99.1`(open→close 墙钟)口径需与分段语义对齐;
- `bytes=19.4 MB / ~100 s ≈ 0.19 MB/s`,与 `--kbps 4000`(0.49 MB/s)有出入——
  待 S3 对账(GOP 头/填充计法),确认事件体积模型后再定 `disk_min_mb`;
- 分段(max-event-sec)、cooldown、水位未实现(S3/S4)。

## 文件

- `gov_test.log` — 服务端全量日志
- `gov_test-01.json` — 流式事件的首份 JSON(含新字段)
