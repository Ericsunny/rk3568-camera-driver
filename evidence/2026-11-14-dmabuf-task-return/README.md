# DMA-BUF 输入任务归还与叠字对照

2026-11-14 在 RK3568 板端继续评估局部 V4L2→MPP DMA-BUF 共享。**进展：板端 MPP 输入任务队列可返回提交的输入帧，实时采集探针能在核对该返回后再 QBUF。限制：IMX415 的 120 帧叠字对照出现输出不一致，根因未定位，现役复制路径未替换。**

## 环境与证据

- 板端 #62，现役 evrec SHA-256 `620f67a160df04d238e57cc8cd26e2bdf32328a611329f9848714a46b7b99509`；MPP 库 SHA-256 `9db8192bf6d4bd927a9b48df5a4154af81fd72975a3ca180dab26ad1455d9323`、软件包版本 `1.5.0-1`。[环境记录](environment.json)
- 仓库工作树 HEAD `3a9f9c09becffe540da2a5dd433802fe4d7a8456`；另有既存未提交修改。测试程序在板端私有新目录独立编译，未覆盖现役二进制、配置或内核。
- 每次现场测试前保存方案和检查结果。码流及画面仅留板端 `/root/zc-input-return-20261114-5QFN6U`，未放入仓库。实验结束后两路 G_FMT 恢复为测试前值，3A active，未发现测试进程：[恢复记录](restore.json)。
- [元数据原始归档](metadata-export.tar.gz)包含探针源码、编译结果、每组计划、进程输出、帧哈希、内核日志增量及 SHA-256 清单，不含视频或图片；下载时板端与电脑 MD5 均为 `24bacf2abee847fce2b3cb8e6d87ae47`。

## 发现并修正输入帧探针位置

11 月 13 日的探针在输出 packet 和输出任务中读取 `KEY_INPUT_FRAME`，没有取得输入帧。本次根据源码线索对输入任务队列做新探针：不接相机，提交一帧合成 NV12，获取输出 packet 后，从输入端取回相同任务和同一输入帧，PTS 对应且退出码为 0。[计划](synthetic-r1/plan.txt) · [输出](synthetic-r1/run.log) · [返回码](synthetic-r1/result.json)。

因此，旧的“未在输出侧观察到输入帧”记录仍成立，但不能据此说板端没有可用的输入返回路径。本次输入任务匹配是特定板端库和探针下的实测；不等于整个事件服务已经完成集成。

## 连续帧与 OSD 初步对照

独立探针对同一采集帧先后运行复制输入和 DMA-BUF 输入两套 MPP 上下文；每次检查返回任务、帧指针、PTS 和 buffer 后再 QBUF。关闭检测、RTSP 和事件录像，运行日志中的两个 CPU 计时属于同一双编码探针，不代表产品 CPU 开销。

| 相机 / 配置 | 实际内容 | 检查与结果 |
|---|---|---|
| IMX415 1920×1080，OSD 关 | 30 帧 | 30 次输入返回、30 次 QBUF；两路输出各解码 30 帧、逐帧哈希一致：[日志](live-imx-r1/run.log) · [对照](live-imx-r1/compare.json) |
| OV8858 1280×960，OSD 关 | 30 帧 | 30 次输入返回、30 次 QBUF；两路输出各解码 30 帧、逐帧哈希一致：[日志](live-ov-r1/run.log) · [对照](live-ov-r1/compare.json) |
| IMX415 / OV8858，OSD 开 | 各 30 帧；共享 buffer 上 CPU 烧字，BEGIN/END 同步，额外整帧 `memcmp` | 两路各 30 帧一致，均有 30 对同步调用：[IMX](osd-imx-r1/compare.json) · [OV](osd-ov-r1/compare.json) |

上述结果是有条件的短测：30 帧结果不能代表长期稳定、NPU/3A 联合负载、真实 RTSP 或事件收尾；逐帧哈希一致也不能单独证明所有 cache 和生命周期场景正确。

## IMX415 120 帧出现不一致

关闭诊断用途的整帧 `memcmp` 后，在启用 OSD 的 IMX415 120 帧试验中，两个 H.264 文件均能解码 120 帧、stderr 为空，但解码图像 **116 帧哈希不同**，第一处为从 0 开始计数的帧 4。测试每帧仍检查输入任务返回并执行一次 QBUF、记录一对 DMA-BUF 同步 ioctl；没有据此把这次计时当作可上线收益。[计划](osd-perf-imx-r1/plan.json) · [日志](osd-perf-imx-r1/run.log) · [对照](osd-perf-imx-r1/compare.json)。

对已保存码流做区域哈希，OSD 区域和远离 OSD 的区域均在相应帧出现差异：[离线对照](osd-perf-imx-r1/crop-analysis.json)。编码预测和码率控制也可能传播差异，因此仅凭解码后区域哈希不能断定原始输入的具体差异位置。

随后做两次定位性对照，保持失败记录而不覆盖：

- **保留整帧输入 `memcmp`：**120 帧的复制/导入输出一致；该额外读整帧使探针计时受到明显干扰，不能作为正常 DMA-BUF 性能。[日志](osd-diag-imx-r2/run.log) · [对照](osd-diag-imx-r2/compare.json)
- **不做 `memcmp`，将 CPU 同步 START 移至读取采集帧之前：**120 帧中 112 帧解码哈希不同，第一处为帧 5。[计划](osd-syncfirst-imx-r1/plan.json) · [日志](osd-syncfirst-imx-r1/run.log) · [对照](osd-syncfirst-imx-r1/compare.json)

这些结果说明当前探针在不同访问方式下出现不同结果；数据不够区分 CPU cache、一致性、编解码状态和其他原因，**不能把相关性当作已确认的根因**。两次不一致试验没有作为通过结果，停止继续开流扩展测试。

## 导出器与内核源码线索

未开流、未 QBUF 的独立探针仅申请 MMAP buffer、导出并读 fdinfo：IMX415 节点返回 `exp_name: videobuf2_dma_sg`，探针结束后格式未变、3A active：[计划](fdinfo-imx-r1/plan.txt) · [原始输出](fdinfo-imx-r1/run.log) · [状态](fdinfo-imx-r1/result.json)。

VM 内核树 HEAD `caaf385a7f9f3d456a91c00cbfddb22f76a45df8` 中，`vb2_dma_sg_dmabuf_ops` 没有设置 `begin_cpu_access` / `end_cpu_access`；dma-buf 通用代码只在 exporter 提供回调时调用对应操作，begin 路径还会等待 reservation fence。[源码范围及完整文件指纹](vm-kernel-excerpts.json)。因此在**该 VM 源码实现**中，ioctl 返回 0 不能单凭返回值证明 exporter 实施了相应 cache 清理；但这也不能单独证明当前图像差异的根因。

VM 当前 `arch/arm64/boot/Image` SHA-256 为 `454b1a002d052a3e6a4513ac790fba308fc46e311f99d45d7afbcad96b46137f`，板端默认 Image 为 `40ec35eccd8e78ecacaee883257f2446d056614c50f649ae2d4988806bb07927`。**两者不同，不能把 VM 源码直接断言为板端正在运行的 Image 的精确实现。**下一步需找到板端 #62 构建源和配置，确认该 exporter 回调与平台 cache 语义，再基于可控输入验证。

## 本轮结论与下一步

未修改或部署现役程序，未合并分支或提交。继续之前需解决：

1. 找到与板端 MPP 和 #62 内核相对应的源码/构建证据，明确 input task 返回与 buffer 同步的契约及失败清理。
2. 在不修改原始证据的前提下，用可控输入复现实验差异并确认原因；不能以整帧 `memcmp` 掩盖差异后宣称性能收益。
3. 若实现 OSD，保证检测、CPU 绘制与 MPP 访问先后和可见性明确；然后验证连续采集、OSD、事件录像、RTSP、异常退出、停止/重开、各相机及净收益。

本轮未运行候选事件录像、RTSP、NPU 和双路并发服务，不作通过声明。历史固定帧数字参见[前次评估](../2026-11-13-dmabuf-evaluation/README.md)，不能当作本轮完整服务性能。
