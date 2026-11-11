# OV8858 DRC 参数链路与画面对照

2026-11-10，#62 内核、RKISP21、2022 版 `librkaiq.so`。本记录对应文件哈希与实际测试条件，不将未提交工作区视为某个 Git 版本。导出文本按 [SHA256SUMS](SHA256SUMS) 核对；场景帧只保存在板端私有目录，**未入库，无法仅凭本仓库重新计算像素统计**。

## 参数从 3A 到 ISP

原 OV8858 参考 IQ 的 `normal/day` 中 `adrc_calib.DrcTuningPara.Enable=0`。在独立 3A 进程下，用 [只读观察器](../../tools/isp_params_observer.c)记录 RKISP 参数缓冲提交前的 `module_en_update`、`module_ens`、`module_cfg_update` 与 `frame_id`。只记录参数头，不记录图像或修改参数；结果为用户态提交与内核 QBUF 返回值，不能单独证明物理寄存器已应用。

| 条件 | 参数节点 | 已映射且成功 QBUF | DRC 启用更新 / 启用位 / 配置更新 |
|---|---:|---:|---:|
| 原 IQ、OV8858 | `/dev/video22` | 182 / 182 | 0 / 0 / 0 |
| 原 IQ、IMX415 | `/dev/video13` | 183 / 183 | 0 / 0 / 0 |
| OV8858 临时 `Enable=1` | `/dev/video22` | 182 / 182 | 1 / 182 / 181 |

表中是成功 QBUF 的次数，不是唯一视频帧数；节点号为当时的媒体拓扑。原始 [关闭记录](drc-params-observation-v4-20261110/qbuf.log)、[启用记录](drc-enable-on-20261110/qbuf.log)可用 [分析脚本](../../tools/analyze_isp_params_trace.py)复核。两次短采的退出码均为 0；临时 IQ 试验后原 IQ 和 3A 均恢复。

采流时读取的 ISP 状态，原配置为 [`HDRDRC OFF`](drc-proc-off/rkisp-proc.txt)，临时开启为 [`HDRDRC ON`](drc-enable-on-proc-20261110/ov8858/rkisp-proc.txt)。#62 内核 `procfs.c` 通过 `rkisp_read(..., false)` 获取该值；两个虚拟 ISP 共用硬件时该接口可能读取驱动软件缓存，因此这里称为**驱动报告的状态**，不冒称逐帧物理寄存器读回。

此前将该现象一概解释为“DRC 无法下发”不准确：对本次 OV8858 参考 IQ，启用开关后可观察到 DRC 参数成功入队及驱动报告 ON。`CamHwIsp21.cpp:364/429` 的启动日志依旧出现；与之行号吻合的参考源码和二进制并未证明同构建，不能仅凭日志判断稳定运行时的 DRC 状态。[先前源码对照与边界](../2026-11-10-imx415-drc-review/README.md)

## 固定场景关—开—关

三臂共用同一 OV8858、参考 IQ、1280×960 NV12 和手动 AE 设置；每臂跳过 120 帧后记录连续 4 帧，测量期间曝光读数均为 1120、模拟增益均为 602。AWB 未锁定，驱动报告的 AWB 增益有轻微漂移。亮度是输出 NV12 的 8 位 Y，全帧统计，非标定亮度。

| DRC 开关 | A1 关 | B 开 | A2 关 |
|---|---:|---:|---:|
| 四帧平均 Y 的均值 | 113.0104 | 113.0195 | 113.1209 |
| Y p50 / p90 / p99（各臂首帧） | 115 / 150 / 189 | 115 / 150 / 189 | 115 / 150 / 189 |
| 驱动报告 `HDRDRC` | OFF | ON | OFF |

该参考 IQ 的 DrcGain 均为 1；此场景中仅启用开关未观察到超出 A1/A2 漂移的显著全帧亮度变化。原始：[A1](drc-manual-aba-20261110/a1/metrics.json)、[B](drc-manual-aba-20261110/b/metrics.json)、[A2](drc-manual-aba-20261110/a2/metrics.json)。

## DRC 增益单变量对照

同一固定场景、相同手动 AE，三臂 DRC 均开启，A1/A2 的 13 个 DrcGain 为 1，B 的 13 个值为 2；两份临时 IQ 的解析结构仅该数组不同。`2` 在 [Firefly 参考版本标注的 1–8 范围](https://gitlab.com/firefly-linux/external/camera_engine_rkaiq/-/blob/f7b676bcbd97dcdc6211ed498345eef5afc7259f/include/iq_parser_v2/adrc_head.h)内，**不是本模组标定值**。曝光与模拟增益三臂读数均为 1120/602；AWB 增益有少量变化。

| DrcGain | A1：1 | B：2 | A2：1 |
|---|---:|---:|---:|
| 四帧平均 Y 的均值 | 112.6943 | 143.5729 | 112.1150 |
| Y p50 / p90 / p99（各臂首帧） | 115 / 149 / 189 | 148 / 176 / 200 | 114 / 147 / 189 |
| Y ≥ 250 的像素占比（四帧均值） | 0.1064% | 0.1047% | 0.1056% |

原始：[A1](drc-gain-aba-20261110/a1/metrics.json)、[B](drc-gain-aba-20261110/b/metrics.json)、[A2](drc-gain-aba-20261110/a2/metrics.json)；[像素统计脚本](../../tools/drc_frame_metrics.py)和每臂控制、ISP 状态、QBUF 日志同目录。B 的亮度变化明显大于 A1/A2 漂移，支持在本次场景中 DRC 参数变化影响图像；由于 AWB 仍有轻微变化、仅一个场景，不能把结果扩大为完整画质评估或声称高光改善。

## 当前配置与恢复

验证后在正常 systemd 3A 环境只把 OV8858 参考 IQ 的 `Enable` 从 0 改为 1，DrcGain 原值 1 和其他字段保持；当前 IQ SHA-256 `1dbb2b17c0244e90c2d35d5ccf88f56fb98c9a2d29be7a5ecd774fd8727cbb0f`。原配置 `b86cca83604f91f061be8228756521510940bcda5e704ea8a1e15e33890fcd4f` 保留在板端 `/root/camera-integration-20261110-build/drc-default-enable-20261110/iq-before.json`。**实验性 gain=2 未设为默认。** 现场正常 3A 下 OV8858 手动录像 156 帧、IMX415 77 帧均完成并退出 0，[OV 驱动状态](drc-default-enable-20261110/ov8858/rkisp-proc.txt)、[交付日志](drc-default-enable-20261110/ov8858/service.log)、[最终哈希](final-status.txt)。OV8858 仍使用其他模组的参考 IQ，并未做本模组 IQ 标定。

默认开启 DRC 后，OV8858 的两个板内回环 RTSP 客户端短测及录像服务退出 0，记录中 RTP 序号缺口为 0；[客户端 A](rtsp-drc-on/client-a.log)、[客户端 B](rtsp-drc-on/client-b.log)。这不是弱网或长时间测试。

随后按默认启动重启，核对 #62 与启用后的 IQ 哈希；系统 3A 自动启动，OV8858 采流 180 帧至 `/dev/null` 退出 0，采流时驱动状态为 `HDRDRC ON`，内核增量检查未发现预设错误关键词。[重启后状态与输出](postboot/start.txt) · [驱动状态](postboot/rkisp-proc.txt) · [采集日志](postboot/capture.log)。完整串口日志仅留本地。

观察器首次启动失败，系统 3A 已恢复；第二版仅记录 MMAP，没有得到 QBUF，不作为有效数据。`strace` 随后显示该旧库传入的 ioctl 号在 64 位上符号扩展；以低 32 位匹配后才得到上表的有效记录。[夹具诊断](tracer-diagnostics/)。
