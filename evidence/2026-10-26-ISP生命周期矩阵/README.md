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

`run_3a_stack.sh 7200 /root/soak3a`:**已完成**——120 有效样本,运行 7200s,**80 个事件全部落盘**(脚本结束行确认),3A 全程 active 未动。RSS 1.25~43.5MB 有界波动(与停 3A 形态的 soak2h 同型,无单调增长),fd 恒 14(一次瞬时 15=事件文件打开竞态,首样本 3=采样器与进程启动竞态伪值)。**诚实口径:该脚本未采样温度列**(与 soak_monitor.sh 的差异),温度证据以 soak2h(40.6~46.1°C)为参考。文件:`soak3a-samples.csv`(列:unix,i,RSS,fd,事件数,rtsp 状态行)。
