# 性能改进与验证

入口留证交付版采用单 IMX415 的 ISP 主副路、连续扫描与联合监听。[当前组合与同板复现](../../evidence/2026-11-20-entrance-release/README.md)。以下双 RGB 槽数据保留为早期实现对照。

- [结果与取舍](project-review.md)：版本、条件、收益及限制。
- [架构与所有权](../overview/architecture.md)：缓冲归还、交接、过载和停止。
- [测量口径](measurement-contract.md)：吞吐、损失、CPU、检测年龄和显示延迟。
- [可复算数据](../../evidence/2026-11-17-single-camera-release/README.md)：CSV/meta、摘要和来源哈希。
- [维护记录](worklog.md)：问题、选择、实现和验证。
- [受管录像保留](recording-retention.md)：原生服务接入、事件保护、重启续清与同板短测。

完整控制记录和影像另行保留；公开数据不包含可识别人员录像或本地操作材料。失败、版本差异和协议偏差保留，不将历史、替身或局部优化扩展为整机证明。
