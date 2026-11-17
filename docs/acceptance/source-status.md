# 源码与验证范围

[架构](../overview/architecture.md) · [量化结果](../improvement/project-review.md)。源码、构建、部署和板端证明分别记录。

| 模块 | 当前实现 | 覆盖与边界 |
|---|---|---|
| IMX415/ISP/3A | 厂商BSP驱动和rkaiq，V4L2 NV12 | 单路有限功能/吞吐/人员场景，不称自研ISP或本次内核部署 |
| OV8858 | 自有subdev、BSP接入、I²C/格式/开关流/PM | 历史出图与双实例联测；参考IQ和CRC限制保留 |
| 编码共享输入 | MPP分配/V4L2导入，身份匹配后QBUF；保留copy | 有限板端对照，减少选定复制，不是全链路零拷贝 |
| 检测输入 | RGA/RKNN RGB共享，pass_through=0 | 有输入输出对照，运行库转换保留，不是native直通 |
| 流水线 | 两槽、单worker、八结果、来源与200ms过滤 | 合成自检、主机策略/ASan/UBSan、ABBA和r3现场；默认关，双路/恢复组合未测 |
| 事件/交付 | 编码预录、有界写队列、独立封装验证和失败状态 | 主机错误路径与板端事件；不证明断电持久性或精密时间线 |
| RTSP | 独立游标/副本、锁外发送、停读断开 | 有限多端与重连；回环地址兼容性有限制 |
| 帧观测 | 有界内存、单调打点、退出输出及截断标志 | 可复算软件指标，不测精确物理响应 |
| camd/mini3a/zerocopy | 独立采集、控制和输入实验 | 各自历史范围，不并为主服务结论 |

[公开软件数据](../../evidence/2026-11-17-single-camera-release/README.md)分开性能r1与后续r3。完整设备日志和影像不随该组公开，主机回归不替代相机、SDK和内核实测。

[历史验收](summary.md) · [环境与历史命令](reproduction.md) · [OV8858接入](../bringup/ov8858-isp-integration.md) · [2026-11-11双路](../../evidence/2026-11-11-dual-followup/event02-natural-close.md)。记录版本和边界不因源码合并自动更新。
