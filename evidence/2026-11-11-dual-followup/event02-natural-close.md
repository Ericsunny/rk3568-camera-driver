# 双路事件录像与自然收尾观察

2026-11-11 约 12:12—12:14，在 #62 内核、板端 `evrec` SHA-256 `620f67a160df04d238e57cc8cd26e2bdf32328a611329f9848714a46b7b99509` 下，以两个独立实例运行人体检测、RTSP 和事件录像。预设与命令见[测试计划](dual-event-noperson-20261111/plan.json)、[IMX 参数](dual-event-noperson-20261111/run/imx415/argv.txt)、[OV 参数](dual-event-noperson-20261111/run/ov8858/argv.txt)。两路录像只保存在板端；本目录不含画面和视频。

两路启动后各发生一次事件，原因未单独确认。两事件完成后的一段观察中没有新事件，随后安排人体进入、停留、离开的测试，两路各打开第二个事件。期间两个 RTSP 客户端持续解码：两路第二个事件 `.part` 同时存在的采样窗口约 58.1—73.0 秒，IMX 客户端帧计数从 429 增至 559，OV 客户端从 446 增至 571。现场动作顺序未能确认，因此不能仅凭日志将检测结果归因于特定人的动作。[逐次采样](dual-event-noperson-20261111/samples.jsonl) · [IMX 日志](dual-event-noperson-20261111/run/imx415.log) · [OV 日志](dual-event-noperson-20261111/run/ov8858.log)

| 第二事件 | 交付状态与记录 | 范围 |
|---|---|---|
| IMX415 | `target_left`，complete，MP4 177 帧；包时间记录显示触发前约 4.47 秒及触发后的包，独立软件解码退出 0。 | 抽样画面的事件前后边缘均可见人体的一部分；因此 `target_left` 表示程序按检测状态结束事件，**不等于画面中已无任何人**。 |
| OV8858 | `target_left`，complete，MP4 159 帧；包时间记录显示触发前约 3.30 秒及触发后的包，独立软件解码退出 0。 | 抽样的前、后两帧未见明显人物，中间抽样帧出现人物；三帧抽样不能代表完整场景和检测准确率。 |

[IMX 事件元数据](dual-event-noperson-20261111/run/imx415/event_20261111-121248-02.json) · [IMX 交付](dual-event-noperson-20261111/run/imx415/event_20261111-121248-02.delivery.json) · [OV 事件元数据](dual-event-noperson-20261111/run/ov8858/event_20261111-121248-02.json) · [OV 交付](dual-event-noperson-20261111/run/ov8858/event_20261111-121248-02.delivery.json) · [两路 MP4 解码记录](dual-event-noperson-20261111/event02-decode.txt)

为核对动作，从两路 MP4 分别抽取前、中、末各一帧进行本地查看；抽帧包含人物，已从板端和电脑临时目录删除，不随记录公开。画面和单次事件不能证明模型准确率，也不证明两路帧同步或长时间稳定。录像封装使用包时间跨度估计平均帧率，不保留逐帧采集时间。

停止时两个服务退出 0、3A active、采集节点空闲；客户端由测试停止信号结束，返回 255，不能写成自然退出 0。[结束状态](dual-event-noperson-20261111/watch-status.json)。

本轮前一尝试中，两路现场画面均可见人物，镜头随后调整；为避免对无人基线错误归因，已停止并将其另存，不计入第二事件的受控基线。[前次日志](dual-event-userready-20261111/run/ov8858.log) · [前次结束状态](dual-event-userready-20261111/watch-status.json)。两轮日志与元数据包 [noperson-evidence-raw.tar.gz](noperson-evidence-raw.tar.gz) 的双端 SHA-256 为 `9c192e4a2a403c2cf0e9fb0b0413b3d5e4b850c07485fcd96c7dca5857993413`。
