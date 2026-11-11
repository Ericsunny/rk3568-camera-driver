# 文档索引

从 [项目首页](../README.md)了解双摄链路与能力边界；下面按阅读目的查找。历史实验对应记录中的版本和环境，不自动代表当前板端状态。

## 了解系统

- [系统设计与运行基线](overview/final-system.md)：功能范围、模块分工、设计取舍和验证状态。
- [OV8858 ISP 接入与问题定位](bringup/ov8858-isp-integration.md)：驱动契约、接入障碍和候选验证。
- [相机采集与软件链路](overview/architecture.md)：两路传感器、采集、录像与推流之间的关系。
- [双摄采集记录](pipeline/dual-camera.md)：两路格式、采集实验及结果边界。
- [双路并发短测](../evidence/2026-11-10-dual-concurrency/README.md)：采集、编码、RTSP、手动事件和双路检测观察。
- [双路检测状态采样](../evidence/2026-11-11-dual-followup/README.md)：设备等待、帧进展和测试范围。
- [历史结果与验证范围](acceptance/summary.md)：重要验收记录及尚未覆盖的内容。

## 驱动开发与问题定位

- [I²C 物理口和总线排查](bringup/i2c-debug.md) · [获取第一帧](bringup/first-frame.md)
- [OV8858 驱动与厂商驱动对照](bringup/driver-comparison.md) · [开发环境与操作记录](bringup/hardware-guide.md)
- [内核问题记录](stability/kernel-issues.md) · [启动故障与恢复](runbook/boot-recovery.md)

## 录像、推流与图像

- [事件录像实现](pipeline/event-recording.md) · [事件治理](pipeline/event-governance.md) · [RTSP 多客户端](pipeline/rtsp-multiclient.md)
- [白平衡实验](image/awb.md) · [图像质量](image/image-quality.md) · [高光与动态范围](image/dynamic-range.md) · [HDR 实验](image/hdr-research.md)
- [OV8858 DRC 参数与效果验证](image/drc-validation.md)：参数提交、固定场景对照和参考 IQ 限制。
- [mini3a 控制诊断](../mini3a/README.md)：AE、参数通路与独立实验的验证范围。
- [启动与服务](deployment/service.md) · [稳定性与可靠性](stability/README.md)

## 复现与历史材料

- [验收环境和历史命令](acceptance/reproduction.md)：运行前重新核对设备和版本，不直接照抄旧命令。
- [当前事件服务运行记录](../evidence/2026-11-08-event-service/README.md)：配置、指纹及主机/板端覆盖。
- [当前源码与验证范围](acceptance/source-status.md)：按模块说明实际覆盖及限制。
- [旧编号对照](overview/document-history.md)：查找历史记录中提到的文档编号。
