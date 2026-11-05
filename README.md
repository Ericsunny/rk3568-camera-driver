# RK3568 Camera：驱动、采集与流媒体

基于 RK3568 的双摄工程：编写 OV8858 V4L2 subdev 驱动，调通 IMX415 ISP 与 OV8858 CIF 采集，追踪内核故障，并实现事件录像与 RTSP。每项结论都区分历史版本、当前板测和仍未验证的部分。

[面试导览](docs/42-面试导览.md) · [本次板测复核](docs/43-板测复核报告.md) · [开发方式](CONTRIBUTING.md) · [完整历史与归档索引](docs/archive/README.md)

## 一条主线

| 阶段 | 工作与本次可核对的数字 | 原始记录 |
|---|---|---|
| 硬件链路与点亮 | 同一块板的单路与双路短测中，OV8858 采集工具均报告 **30.02 fps**；这里只比较采集速率。 | [双摄单路/并发输出](evidence/2026-11-05-板测复核/W3-session.log) |
| Sensor 驱动 bring-up | 本次运行中的历史 `#52` 镜像，OV8858 CIF 首帧为 **2,506,752 B**；当前仓库的后续驱动改动未部署、未板测。 | [CIF 捕获输出](evidence/2026-11-05-板测复核/W2.stdout) |
| 单链路 3A 与图像 | 现役 rkaiq 保持运行时，IMX415 连续取得 **2 帧**，内容哈希不同；这不等于 AE 收敛或画质通过。 | [两帧捕获输出](evidence/2026-11-05-板测复核/W1.stdout) |
| 事件录像与 RTSP | 事件收尾的主机替身覆盖 **5 类**文件故障；真实设备收尾及当前 RTSP 复核尚待执行。 | [主机测试原始输出](evidence/2026-11-05-板测复核/host-tests.log) |
| 稳定性与回归 | 本次 **100 次**开关流均取得首帧；内核同时记录重复错误字样，完整稳定性结论待定。 | [逐次结果](evidence/2026-11-05-板测复核/W5-board-logs/W5.stdout) · [内核日志](evidence/2026-11-05-板测复核/W5-board-logs/kernel-new.log) |

## 五分钟阅读路径

1. [面试导览](docs/42-面试导览.md)：从 C、多线程、V4L2 和 RTSP 对应到代码与追问。
2. [物理口与 I²C 排障](docs/09-L1A-I2C调试复盘-物理口与总线映射.md)：查看假设怎样被现场证据纠正。
3. [内核问题闭环](docs/28-内核问题闭环.md)：从 TRY_FMT、compat 到回归边界。
4. [板测复核报告](docs/43-板测复核报告.md)：查看本次命令、原始输出、差异和未完成项。

## 开发方式与边界

AI 辅助脚手架、测试替身和资料对照；人负责硬件现场核对、方案取舍、验收与回退决定，并对需批准的操作逐次确认。[具体例子](CONTRIBUTING.md)

OV8858 走 CIF RAW10，只有 mini3a 软件闭环；没有完整 ISP Tuning / IQ 调优。没有 24 小时连续长稳、四客户端满载长跑、功耗实测或 Android Camera HAL3 实操。输出 native 字节一致性尚未闭合；带流 suspend 的保护不等于真实唤醒已修复。最新驱动源码尚未部署或板测。历史失败记录及完整边界均保留在[归档索引](docs/archive/README.md)。

<details>
<summary>探索性实验（学习性质，非主线交付）</summary>

包含 HDR、双 lane DOL、RAW10 合成、mini3a、零拷贝三模式和 NPU 检测样张；其中部分结论未完成当前版本复核，历史缺失的原始附件不作为已复核依据。入口见[归档索引](docs/archive/README.md)与 [RAW10 合成工具说明](tools/raw10_stats.md)。

</details>

<details>
<summary>仓库结构与数量口径（2026-11-05）</summary>

`driver/`：sensor 驱动与内核补丁；`tools/`：采集、事件录像、RTSP 与测试；`zerocopy/`：实验链路；`mini3a/`：软件闭环；`docs/`：主干与归档；`evidence/`：原始记录，只增不改。`docs/` 共 36 份 Markdown（主干 12、归档 24；2026-11-05 工作分支快照）。当前工作分支的数量见 [复核报告](docs/43-板测复核报告.md)，main 以合并后的 `git rev-list --count HEAD` 为准。

</details>
