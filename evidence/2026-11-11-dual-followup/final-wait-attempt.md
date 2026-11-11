# 双路自然收尾联测等待记录

2026-11-11 约 10:49—10:52 进行双路检测与 RTSP 联测，目标是受控观察无人、入镜、离开后的自然收尾。两路预录就绪后启动客户端；到达预设的 180 秒时限时两路未记录事件，程序按时限停止。不能仅凭日志判断等待期间是否有人进入画面，本轮不构成自然收尾或人体检测通过的证据。

最后一次采样中两路客户端分别解码 1350、1356 帧，事件 `.part` 均为空；结束时两路服务退出 0、3A active、设备空闲。客户端收到结束信号后返回 255。[预设条件](dual-event-final-20261111/plan.json) · [逐次采样](dual-event-final-20261111/samples.jsonl) · [结束状态](dual-event-final-20261111/watch-status.json) · [IMX 日志](dual-event-final-20261111/run/imx415.log) · [OV 日志](dual-event-final-20261111/run/ov8858.log)

日志包 [final-evidence-raw.tar.gz](final-evidence-raw.tar.gz) 双端 SHA-256 均为 `5a46b15d9edb650b8f52e9af28f7d1f57109c8f4508ade05cb969c0f52e57619`。本包不含现场画面或录像。
