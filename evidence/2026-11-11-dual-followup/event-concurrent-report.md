# 双路人体检测、录像与 RTSP 并发观察

2026-11-11，在 #62 内核和两路独立 `evrec` 实例下进行有限现场验证。使用未提交工作区：板端 `evrec` SHA-256 `620f67a160df04d238e57cc8cd26e2bdf32328a611329f9848714a46b7b99509`；预设条件及精确参数分别见 [联测计划](dual-event-controlled-v2-20261111/plan.json)、[运行参数](dual-event-controlled-v2-20261111/run/imx415/argv.txt)和 [OV 参数](dual-event-controlled-v2-20261111/run/ov8858/argv.txt)。用户视频未导出或公开；日志和元数据传输包 [event-evidence-raw.tar.gz](event-evidence-raw.tar.gz) 的双端 SHA-256 均为 `ae60b02a06490523ce23f464489b30073a913ccc3706edaabcbc48daad2bf5c5`。

## 首次联测：监视条件误报

无人阶段，两路 RTSP 客户端均在解码，尚未出现事件；由于 OV8858 进程连续多个采样点为 `D`，监视器按原条件提前停止。[状态记录](dual-event-controlled-20261111/watch-status.json)和[逐次采样](dual-event-controlled-20261111/samples.jsonl)显示，在触发停止前约 3 秒的采样窗口中，该路解码帧数从 498 增至 523，ISP 输出计数也在增长。多次采样均落在 `D` 并不能证明进程在采样之间连续阻塞。两路进程均正常退出，3A 保持 active。

后续另起新的一轮，并在开测前将保护条件改为检查 `D` 采样和应用进度、另设编码/ISP 停滞保护；[新计划](dual-event-controlled-v2-20261111/plan.json)和[监视代码](dual-event-window-v2.py)保留。新条件的主机替身测试只验证监视逻辑，不代替硬件测试。

## 第二次联测：双路触发和推流重叠

- 两路无人阶段未产生事件；用户入镜后两路日志均出现检测触发并打开录像。实验到预定 180 秒上限后停止，两个事件的关闭原因均为 `sigterm`，**不算目标离开后自然收尾的验证**。[IMX 日志](dual-event-controlled-v2-20261111/run/imx415.log) · [OV 日志](dual-event-controlled-v2-20261111/run/ov8858.log) · [监视状态](dual-event-controlled-v2-20261111/watch-status.json)
- 两个事件 `.part` 均存在的约 6 秒采样窗口内，两路客户端解码计数分别从 1186 增至 1241、从 1204 增至 1259；两份最终包记录中，触发后分别有 63 和 59 个不同帧序号。事件进行期间 `.part` 大小在采样中没有变化，源码使用 4 MiB 用户态批量写缓冲，不能单凭文件大小推断没有接收后续编码包。[进程与帧采样](dual-event-controlled-v2-20261111/samples.jsonl) · [IMX 包记录](dual-event-controlled-v2-20261111/run/imx415/event_20261111-091830-01.packets.csv) · [OV 包记录](dual-event-controlled-v2-20261111/run/ov8858/event_20261111-091830-01.packets.csv)
- 两路交付结果均为 complete，分别记录 91 和 94 帧；另用软件解码器完整解码生成的 MP4，均返回 0 且无错误输出。客户端结束码 255 来自测试结束时发送的 SIGINT，不能作为自然退出 0 描述。[IMX 交付](dual-event-controlled-v2-20261111/run/imx415/event_20261111-091830-01.delivery.json) · [OV 交付](dual-event-controlled-v2-20261111/run/ov8858/event_20261111-091830-01.delivery.json) · [MP4 复核](dual-event-controlled-v2-20261111/mp4-decode-verification.txt) · [停止状态](dual-event-controlled-v2-20261111/watch-status.json)

该轮证明在本次有限场景中，两路人体检测触发录像期间，两个 RTSP 客户端都在解码并且两路录像可收尾和解码；不证明复杂场景准确率、长时间稳定性或丢帧率。MP4 的时间轴仍为平均帧率近似，不是逐帧采集时间戳。

## 后续尝试：不计入联合验收

另一轮观察达到时限后自动退出，未启动 RTSP 客户端，也没有完整记录人员按提示入镜和离开的受控过程；日志虽包含 `target_left` 关闭事件，无法将其归因于受控人员离开。该次只保留为过程结果，不计入双路自然收尾验收。[状态](dual-event-natural-20261111/watch-status.json) · [IMX 日志](dual-event-natural-20261111/run/imx415.log) · [OV 日志](dual-event-natural-20261111/run/ov8858.log)；[日志包](natural-evidence-raw.tar.gz) SHA-256 `767f6b02e0dc791b1b9fa0f862e119037e1f11c063d36d48f0909c5170f72f5b`。
