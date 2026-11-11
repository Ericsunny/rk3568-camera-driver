# 双路自然收尾复测：IMX415 视野仍有人

2026-11-11 约 11:13—11:16，再次进行双路无人、人体进入、离开及 RTSP 同时解码测试。两路预录就绪且无人基线没有事件，随后两路均发生检测并开启录像，两个客户端解码计数也持续增长。OV8858 事件的交付状态为 complete，关闭原因为 `target_left`。[逐次采样](dual-event-verified-20261111/samples.jsonl) · [IMX 日志](dual-event-verified-20261111/run/imx415.log) · [OV 日志](dual-event-verified-20261111/run/ov8858.log) · [OV 交付结果](dual-event-verified-20261111/run/ov8858/event_20261111-111322-01.delivery.json)

IMX415 则持续出现人体命中，录像先按最大时长分段，最后在测试时限到达时以 `sigterm` 收尾；不能算该路自然收尾通过。为了解持续命中，现场在板端从最后一段 MP4 抽帧，临时检查发现画面右侧仍有人体的一部分；该画面无法证明在此前每一刻都有人，但足以说明不能将末段持续命中直接归为无人误检。临时抽帧已从板端和电脑删除，录像仅留板端。[IMX 最后一段交付结果](dual-event-verified-20261111/run/imx415/event_20261111-111322-01-s3.delivery.json) · [进程结束状态](dual-event-verified-20261111/watch-status.json)

其后另一轮尝试的 IMX415 在启动后即产生事件，现场实时预览中右侧仍有人；未获得有效的双路无人基线，不计入双路自然收尾验收。OV8858 有 `target_left` 结果。两路最终均正常退出，3A 仍为 active，采集节点空闲；该轮临时预览也已删除。[第二轮 IMX 日志](dual-event-clean-20261111/run/imx415.log) · [OV 日志](dual-event-clean-20261111/run/ov8858.log) · [结束状态](dual-event-clean-20261111/watch-status.json)

日志和事件元数据包 [verified-clean-evidence-raw.tar.gz](verified-clean-evidence-raw.tar.gz) 双端 SHA-256 均为 `869504e3b2f657005390b1c0ac2c68804963086a0a638f95c7661e63cd945ca7`，不包含画面或录像。
