# 文档索引

## 当前主服务

推荐入口为 `camera_demo imx415-isp entrance`。从[发布验收与复现命令](../evidence/2026-11-20-entrance-release/README.md)可进入副路 A/B、故障与重新构建数据。

- [架构与数据链路](overview/architecture.md)：硬件/控制通路、进程、线程、所有权和失败行为。
- [运行与配置](overview/final-system.md)：依赖、默认值、入口、观测和停止。
- [早期双缓冲与编码共享结果](improvement/project-review.md)：两项对照、人员事件、新鲜度与限制。
- [可复算数据](../evidence/2026-11-17-single-camera-release/README.md)：CSV/meta、摘要和来源哈希。
- [源码验证范围](acceptance/source-status.md) · [改进记录](improvement/README.md)

## 驱动与历史路径

- [OV8858接入](bringup/ov8858-isp-integration.md) · [I²C排障](bringup/i2c-debug.md) · [首帧](bringup/first-frame.md)
- [驱动对照](bringup/driver-comparison.md) · [硬件操作](bringup/hardware-guide.md)
- [启动异常与恢复](runbook/boot-recovery.md) · [内核问题](stability/kernel-issues.md)
- [事件录像](pipeline/event-recording.md) · [事件治理](pipeline/event-governance.md) · [RTSP](pipeline/rtsp-multiclient.md)
- [编码输入](overview/encoder-input-choice.md) · [检测输入](overview/npu-input-choice.md)
- [服务](deployment/service.md) · [可靠性](stability/README.md) · [DRC](image/drc-validation.md) · [mini3a](../mini3a/README.md)
- [历史验收](acceptance/summary.md) · [复现环境](acceptance/reproduction.md) · [双路](pipeline/dual-camera.md) · [旧编号](overview/document-history.md)

历史双路与独立工具结果保留各自版本和范围，不自动成为当前单路流水线、恢复或长期运行证明。
