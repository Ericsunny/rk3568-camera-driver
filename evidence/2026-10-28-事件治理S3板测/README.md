# 2026-10-28 事件治理 S3 板测(T1/T2/T3)

提交 `285923c`(A 语义闭合)+ `c94006c`(观测打印)。

## 结果总表

| 测试 | 场景 | 结果 |
|---|---|---|
| **T1** 持续目标 + `--max-event-sec 8` + 中途 SIGTERM | 4K 检测,95s | **PASS**:11 段同一 `event_id=1`、`segment_index` 0..10 连续、每段 8.06~8.27s、每段 bytes 量级一致(1.10~1.20MB,首段 1.60MB 含 pre-roll)、SIGTERM 末段完整落盘 |
| **T2** 幽灵事件回归(无 `--max-event-sec`) | 60s | **PASS**:1 次触发→1 事件,`duration_ms=60207` 媒体跨度精确,无触发前无 open(open-done=0) |
| **T3b** 有人在(观测版) | 45s | 命中率 **95~100%**(峰值 score 0.798),streak 峰值 193 → `alive` 永真,事件持续续录 |
| **T3c** 盖 OV8858 镜头对照 | 15s | 命中率仍 100%,NPU 输入均值不变(mean≈116) → **盖错了镜头:检测链在 IMX415(J10)/video5,OV8858 在 J11 根本不在此链** |
| **T3d** 人离开(IMX415 视野无人) | 60s | **PASS**:12 行统计全部 `命中 0/61 帧`,峰值 0.373~0.508(偶发过 0.45 但凑不满连续 3 帧,streak 恒 0),**0 触发、0 事件、0 文件**——`target_left` 语义前置生效(未触发即无事件) |

## bytes/duration 对账(账 2,T1 实测)

- 续段:~1.13MB / 8.1s ≈ **0.14 MB/s ≈ 1.1 Mbps**(`--kbps 4000`、实测 7.3fps;
  MPP RC 按帧发预算,实际码率随 fps 缩水——与 docs/37 §3.3 换算一致);
- 300s 满长段 ≈ **42MB**,`disk_min_mb=500` 可容 ~12 段,余量充足 → S4 默认值可定 500。

## 检测链说明(重要背景)

事件录像跑在 **IMX415(J10,video5,ISP 链)** 上——只有它经 ISP 才有 H264 编码输入;
OV8858(J11)不在检测链。T3c 盖 OV8858 镜头不影响检测,属预期。

## 检测灵敏度遗留(非状态机缺陷)

T3b/T3d 对照显示:IMX415 视野中的**屏幕光/反光**会在人走动时造成持续命中
(T3b 峰值 0.798),人离开后归零(T3d 峰值 0.51、多数 <0.45)。生产部署需
`--det-score` 调优或 IQ 优化,与事件状态机无关。

## 数据完整性说明(如实)

- T1 的 11 份 JSON + T2 的 1 份 JSON **完整保存在 `samples-json.txt`**(当晚抄录);
- T1/T2 的**服务端 stdout 日志**被后续清理命令误删,未能入库 —— 结论数据
  (JSON)无损,stdout 仅丢失周期性 rtsp/fps 报告行;
- T3b/T3c/T3d 的 log 完整入库。

## 文件

- `samples-json.txt` — T1 全部 11 段 + T2 事件 JSON 全文(核心判定数据)
- `t1-sustained.log` 缺失(误删,见上);`t2-ghost-regress.log` 缺失(同)
- `t3b-occupied-stats.log` / `t3c-lenscover-control.log` / `t3d-absent-stats.log` — 观测版三轮
