# V4L2 与 MPP 共享输入验证

2026-11-14，在 RK3568、Linux 4.19.232 #62、现有 MPP/RGA/RKNN 环境中，尝试消除事件录像程序中从采集帧到编码输入缓冲的一次整帧 CPU 复制。当前源码提供 `--encoder-input dmabuf`，默认仍为 `copy`；启动器可用 `ENCODER_INPUT=dmabuf` 显式选择。没有隐式回退；内存申请、同步或输入归还检查失败会报错退出。

结论：**隔离测试和有限业务短测显示此路径可用，纯录像的进程 CPU 时间下降；检测开启时收益较小。OV8858 在最终版本的一次注入恢复后出现 CSI-2 CRC，根因未定，故不将共享模式设为默认，也不把短测称为长期验证。**

## 实现与所有权

之前的尝试由 V4L2 MMAP 导出 DMA-BUF 给 MPP；CPU 叠字后，IMX415 的一次 120 帧对照有 116 帧解码哈希不一致，原因未查明。[原记录](../2026-11-14-dmabuf-task-return/README.md)

本次改为 MPP 分配 buffer，V4L2 按 `V4L2_MEMORY_DMABUF` 导入相应 fd；板端 fdinfo 显示该分配器实际为 `rockchipdrm`。这符合 [V4L2 DMA-BUF 导入接口](https://docs.kernel.org/userspace-api/media/v4l/dmabuf.html)的使用方式，不代表其他 BSP 必定支持。采集完成后，RGA 通过 fd 读帧；CPU 叠字以 DMA-BUF START/END 同步包围；MPP task 返回的任务、帧、PTS 和 buffer 全部匹配后才 QBUF。检查失败先退出，异常收尾先复位/释放 MPP 再释放采集缓冲。

ISP 队列 `QUERYBUF` 返回的长度可能比 `S_FMT.sizeimage` 大。IMX415 1080p 本次分别为 **3,133,440 B** 与 **3,110,400 B**；若按后者分配，首次 QBUF 返回 EINVAL。修正为按队列要求长度分配后通过。[失败记录](board/imx-import-r2/run.log) · [修正后](board/imx-import-r3/run.log)

`copy_bytes=0` 只统计应用从 V4L2 输入到 MPP 输入的显式整帧 CPU 复制；RGA、ISP 和编码器仍会访问内存，编码包、RTSP 私有数据和录像写入仍有复制。不称为整机或全链路零拷贝。

## 画面及业务验证

| 场景 | 结果 | 范围 |
|---|---|---|
| 两种相机，OSD 开启 | MPP 分配的同一采集帧分别供复制/共享两套编码器使用；每路 120 帧均完整解码且逐帧一致 | 以显式软件解码复核：[IMX415](board/imx-import-r3/compare-software.json) · [OV8858](board/ov-import-r1/compare-software.json)。不涵盖全部动态场景或长期运行。 |
| RGA 输入 fd | 对原 V4L2 MMAP 采集 buffer，同一帧分别经虚拟地址和导出 fd 预处理；每路 30 帧 RGB 比较均一致 | [IMX415](board/rga-equiv-imx/run.log) · [OV8858](board/rga-equiv-ov/run.log)。该对照使用的是同一个 V4L2 导出 buffer，不是完整比较两种分配器的全部情况。 |
| 单路录像、检测、RTSP | IMX415 最终 v3 候选开启检测、OSD、两个 RTSP 软件解码客户端；手动事件正常交付，客户端各解码 53 帧，触发时均在播放 | [客户端](board/final-v3-imx/rtsp-result.json) · [交付](board/final-v3-imx/validation.json) · [H.264/MP4 软件解码逐帧比较](board/final-v3-imx/software-verify.json)。检测阈值设为 1.1 排除场景相关自动触发，不验证人体准确率。 |
| 双路并发 | v2 候选两路同时进行共享输入、RGA/NPU、OSD、手动事件、各两个 RTSP 软件解码客户端，触发时客户端在线；两段事件完成 MP4 交付 | [IMX](board/dual-npu-imx/rtsp-result.json) · [OV](board/dual-npu-ov/rtsp-result.json) · [RGA 硬件提交跟踪](board/dual-npu-trace-summary.json)。v3 随后补了空句柄/fd 检查，双路并发未在 v3 重新执行。仅为短时手动事件，不代表双路自动人体事件或长稳。 |
| 异常与退出 | 主机替身检查不同任务、帧、PTS、buffer、同步重试和超时；最终 v3 板端注入无效 fd，命中后进程非零退出且没有回退 | [主机测试](host-validation-v3.log) · [板端注入结果](board/fd-failure-summary.json) · [进程日志](board/final-invalid-fd/run.log)。v2 上两路分别做过 SIGTERM 收尾和模拟 DQBUF 失败恢复，不代表所有错误场景。 |

最初两位 RTSP 客户端在事件触发前退出，未满足重叠要求；后续延长客户端运行并从日志核对触发时两客户端均在播放。另发现板端 FFmpeg 默认选用 `h264_rkmpp`，会额外初始化 RGA；后续对照明确使用 `-hwaccel none -c:v h264`。使用旧录像的受控检查中，默认硬解产生一次版本探测日志，而显式软件解码没有：[记录](board/decoder-probe.json)。旧中止记录保留在[此处](board/rtsp-detect-imx-r1/result.json)。

## CPU 结果

两种模式在相同程序、相机尺寸、OSD 和手动触发条件下，按 `copy、dmabuf、dmabuf、copy、copy、dmabuf` 各运行三次。指标为**整个录像进程**的用户态＋内核态 CPU 时间，除以处理帧数，含启动开销，不含 RTSP 客户端和 MP4 交付进程；每组仅约 8–9 秒。录像场景数据使用 v1 候选，检测场景使用最终前的 v2 候选；v3 的检查增强后未重复全部性能套件。

| 场景 | 相机 | copy 中位数 ms/处理帧 | dmabuf 中位数 ms/处理帧 | 本次相对差异 |
|---|---|---:|---:|---:|
| OSD 开、检测关 | IMX415 | 4.94 | 3.68 | 约 -25.5% |
| OSD 开、检测关 | OV8858 | 3.09 | 2.21 | 约 -28.4% |
| OSD 与检测开 | IMX415 | 27.98 | 26.17 | 约 -6.5% |
| OSD 与检测开 | OV8858 | 24.96 | 24.37 | 约 -2.4%，样本波动较大 |

[录像原始汇总](board/performance-summary.json) · [检测原始汇总](board/performance-detect-summary.json)。检测测试使用阈值 1.1，并以手动信号触发事件；以上不代表整机 CPU、端到端延迟或帧率提高相同百分比，也不代表不同板卡会得到相同效果。

## 未完成和现场状态

最终 v3 的 OV8858 测试在第 20 次 DQBUF 模拟失败并重开采集后，监测到 `ERR1: crc errors`，监视器发送 SIGTERM 后进程返回 0，但整项**按中止处理，不是通过**；此次还未进入计划的事件触发阶段。[程序](board/final-v3-ov-recover-term/run.log) · [内核增量](board/final-v3-ov-recover-term/kernel-new.log) · [监控结果](board/final-v3-ov-recover-term/result.json)。之前复制路径的自写驱动开关流也曾出现同类 CRC；目前不能将其归因于 DMA-BUF、驱动或硬件的某一处。[历史记录](../2026-11-14-ov8858-driver-evaluation/README.md)

因此共享输入仅作为可选候选；后续应独立排查 OV8858 CSI-2 CRC，再以同一内核、模式和恢复方式对复制与共享路径做可控对照。自动人体触发、复杂光照、长时间运行和最终 v3 的双路并发仍未覆盖。

最终源码 `tools/evrec.c` SHA-256 `71718ae633483d0a62932313b7eb1d2eb4faba778a7c30b80c9d2ce01e196ef2`，板端独立候选二进制 SHA-256 `455480b074bfffe964d1b75ab56882f1cd70b80310191a57e39ed5aa6a60d9ee`，双宏 `-Wall` 编译无输出。无宏组合额外编译失败；用更改前源码复核也有同类失败，不是本次新出现，项目 Makefile 的应用目标使用双宏。[基线](board/baseline-no-npu.log) · [候选无宏日志](board/candidate-v2/build-no-npu.log)

[主机回归](host-validation-v3.log)包含所有权替身、ASan/UBSan、启动器和录像相关测试；[补充回归](additional-host-v3.log)包含事件故障注入、检测后处理、MP4 和交付测试。这些替身不代替板端测试。之前 v2 的真实启动器使用 `ENCODER_INPUT=dmabuf` 完成事件交付：[记录](board/launcher-final-imx/result.json)。v3 启动器未重跑。

结束时两路格式恢复，3A active，测试设备与端口空闲，Image 和 DTB 哈希未变化：[状态](board/final-state.json)。板端候选位于独立目录，现役应用和启动配置未覆盖，代码和证据未提交。

[完整文本、源码快照与帧哈希归档](metadata-complete.tar.gz)板端及本机 SHA-256 均为 `f886c5ea6bc062eb5e4066a3df17043cc335f0062c05412dd9c767f785af6d4c`；归档不含视频或图片。最终 v3 的软件解码比对在该归档生成后执行，单独取回并核验 SHA-256 `9136020415879cee2a5dfe25cc84365e787915245e0022c1be2e168238690b93`。实际录像仅留在板端私有目录。
