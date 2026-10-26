# 2026-10-26 ISP 生命周期矩阵(rkaiq_3A × 采集)实测证据

内核 #47,板 YCKJ iTOP-3568。矩阵与归因表正文见 **docs/29 §10**;操作规则修订见 docs/35 §4.1、docs/33 取证前提。

## 文件 → 矩阵格对照

| 文件 | 覆盖格 | 关键读数 |
|---|---|---|
| `m_a4.log` | **A4**(3A 停→直采) | 3 事件全落盘,~14.9fps,SIGTERM 干净退出 → 停 3A 后**无需重启** |
| `m_a1.log` | **M2+A1**(空闲 start 3A→3A 常驻全链) | `检测触发 score=0.745/0.814/0.752`(固定曝光口径 0.61~0.75,不降反升),det_total=217/score_max=0.835,10.3fps(--detect 已知负载),RTSP 服务端就绪 |
| `m4.log` | **A6+A2+A5** | A6:`--fault-sim` 恢复成功(3A active 下重开 video5,30 帧稳定复位),RTSP 实拉 68 帧(sent=69);A2:采流中 `systemctl stop rkaiq_3A` → 在跑流暂时无恙,evrec 退出 STREAMOFF 时 `rkisp_stream_stop id:0 timeout`(文件尾部补录 dmesg 行)→ A5:st_burn 3/3 fail 且复测仍 fail(**新打开者全灭**) |
| `stack3a_test.log` + `samples.csv` | **AC4 一键栈验证** | `run_3a_stack.sh` 150s 实跑:3A active 断言→2 事件落盘→`有效样本 3 行, 运行 180 秒, 事件 2 个, 3A 全程未动` |

## 重启与恢复

A2/A5 wedge 后执行 1 次重启(预算 2 次内):重启后 st_burn 2/2 OK(fail_pct=0.00,首帧 252~264ms)——**唯一恢复路径=重启,已验证**。

## 长稳(启动于矩阵完成时)

`run_3a_stack.sh 7200 /root/soak3a`:2h soak,**3A 常驻形态**(此前 2026-10-25 的 soak2h 是停 3A 形态)。收集:`ssh rk3568 ssh rkboard tail /root/soak3a/samples.csv`。
