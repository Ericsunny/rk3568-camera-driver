# 2026-10-28 事件治理 S3 验证(分段机制)

分支 `feature/event-governance`,S3 内容:元数据保护 + `--max-event-sec` 分段 +
`event_id/segment_index` JSON 字段 + `close_reason: max_duration`。

## 分段机制:工作 ✓

`--max-event-sec 8`,40 秒会话(镜头无人,但流式录制已开):

| 段 | bytes | duration_ms | close_reason | first→last ts |
|---|---|---|---|---|
| 01 | 2.07MB | 8087.1 | max_duration | 825.7→8912.8 |
| 02-1 | 4.01MB | 8149.5 | max_duration | 9071.6→17221.0 |
| 03-1 | 6.09MB | 8085.6 | max_duration | 17395.1→25480.7 |
| 04-1 | 8.05MB | 8156.5 | max_duration | 25685.1→33841.6 |
| 05-1 | 9.47MB | 6085.1 | sigterm | 34036.4→40121.5 |

- 每段时长 ≈8.1s(与 `--max-event-sec 8` 精确吻合);
- **IDR 边界衔接**:相邻段 first/last 时间连续(8912→9071 等仅隔 GOP 间隙),无丢帧;
- fps 全程 7.2 稳定,dmesg 零错误,SIGTERM 段完整落盘(`close_reason: sigterm`)。

## 暴露的两个语义缺口(如实记录,S3 收尾待产品决定)

1. **每段都是新 `event_id`(2987/2995/3003/3011),`segment_index` 恒 0** ——
   与"event_id 跨段不变、segment_index++"的设计不符。根因:切段重开路径
   `evw_close → g_events++ → evw_open` 走了"新事件"命名与 ID 分配分支。
2. **无触发也在录**(trigger_seq=0)——切段重开形成了"永远在录"的循环:
   流式架构下"录制中"与"事件中"变成两个概念,切段重开必须回答
   "现在还有没有事件"(目标离开判定应同样适用于切段重开的门控)。

## 过程修复

- 分段块曾放在触发块之后,而持续目标下触发块每帧 `continue`(元数据保护短路)
  → 分段永远不可达;已把分段块上移到触发块之前(每帧必达)。

## 文件

- `s3u.log` — 服务端全量日志
- `jsons.txt` — 5 份 JSON 全文
