# 25 · HDR 可行性预研（RK3568 + IMX415）

> 目标：在不改变现有主线（零拷贝 / 双摄 / NPU / 3A）的前提下，判断这块板子能不能做 HDR，能的话代价多大、先做哪一步。
> 所有结论都来自本机实测或本仓库源码/镜像内文件，命令附在文末。
> 日期：2026-10-22　分支：main　板卡：YCKJ iTOP-3568（RK3568，内核 `#33`）

---

## 1. 结论摘要

| 问题 | 结论 |
|---|---|
| 这块板子能不能做 HDR | **能，但当前不能"开个开关就好"**：传感器驱动里的 HDR 模式表被 `#if 0` 编译掉了，生效的只有 linear 模式 |
| ISP / 3A 侧支不支持 | **支持**：rkisp 驱动有 HDR 采集与回读路径，2022 版 rkaiq 带完整 amerge/AHDR 算法，IQ 里 `amerge_calib` 也在 |
| 最快的落地方式 | **软件多帧融合（路线 A）**：零内核改动，已用本机实测跑通，高光溢出 5.57% → **0.00%**，暗部占比 75.8% → **9.35%** |
| 真正的硬件 HDR | 需要**新增 2-lane HDR 寄存器表**（路线 B）或**改 4-lane 走线**（路线 C），工作量分别约 2 天 / 不可控 |
| 建议 | 先做 A 并集成进应用层；B 作为下一个可选里程碑；C 只在硬件确认支持 4-lane 后再谈 |

---

## 2. 现状盘点：四层各自的 HDR 能力

| 层 | 证据 | 结论 |
|---|---|---|
| **UAPI** | `include/uapi/linux/rk-camera-module.h:69-73` 有 `RKMODULE_GET_HDR_CFG` / `RKMODULE_SET_HDR_CFG`；`:355` `enum rkmodule_hdr_mode { NO_HDR = 0, HDR_X2 = 5, HDR_X3 = 6 }`；`:366` `enum hdr_esp_mode` | 接口齐全 |
| **传感器驱动** | `drivers/media/i2c/imx415.c:2502/2507` 实现 GET/SET_HDR_CFG；模式表里 `HDR_X2`/`HDR_X3` 条目都在，**但整段 4-lane 模式表被 `#if 0` 包住（1529–1725 行）**；`#endif` 之后生效的表只剩两条 `NO_HDR`（3864×2192、1944×1096） | **HDR 代码在，表被关掉** |
| **DTS** | `arch/arm64/boot/dts/rockchip/rk3568-yckj-csi2-double.dtsi` 中 `imx415@1a` 的 endpoint：`data-lanes = <1 2>;` | 本板走 **2-lane**，正好落在被关掉的那张表之外 |
| **rkisp 驱动** | `drivers/media/platform/rockchip/isp/capture.c:235-253` 处理 `HDR_FRAMEX2_DDR` / `HDR_LINEX2_DDR` / `HDR_FRAMEX3_DDR` / `HDR_RDBK_FRAME2` 等；`csi.c:277-289` 通过 `RKMODULE_GET_HDR_CFG` 读传感器模式写入 `dev->hdr.op_mode` | **ISP 采集路径支持 HDR** |
| **rkaiq（2022 版）** | `strings /usr/lib/librkaiq.so` 含 `RkAiqAmergeHandle` / `AmergeConfig` / `AmergePreProcess` / `AmergeGetSensorInfo` / `RK_AIQ_ISP_HDR_MODE_2_FRAME_HDR` / `2_LINE_HDR` / `3_FRAME_HDR` / `3_LINE_HDR` / `AecHdrLfrmExecute` / `AecHdrSfrmExecute` / `rk_aiq_user_api_ahdr.cpp` | **3A 算法侧完整支持 HDR** |
| **IQ 文件** | `scene_isp21` 27 个 block，其中 `amerge_calib.MergeTuningPara`（13 点 `OECurve`/`Smooth`）与 `adrc_calib` 都在 | **标定数据已就位** |

一句话：**三层（UAPI / ISP / 3A+IQ）都准备好了，卡在传感器那一层的一张表。**

---

## 3. 三条路线对比

### 路线 A：软件多帧融合（本次已实测）

做法：传感器保持 linear，按"暗/中/亮"三档曝光各抓一帧，在应用层按"曝光适中度 + 局部对比度"加权在线性域融合。

实测（4K，3840×2160，同场景，关闭 3A 避免 AE 覆盖手动曝光）：

| 帧（曝光行数） | 平均 Y | 中位 Y | 高光溢出(≥250) | 暗部(<16) |
|---|---|---|---|---|
| dim（60） | 20.08 | 14 | 0.00% | 75.82% |
| mid（300） | 35.01 | 21 | 2.41% | 20.24% |
| bright（1400） | 72.09 | 50 | 5.57% | 2.06% |
| **fused（三帧融合）** | **54.59** | **40** | **0.00%** | **9.35%** |

- 效果：窗户（高光）保留细节，同时桌面（暗部）不再死黑；见 `evidence/hdr/hdr_fusion_overview.png` 与 1:1 对比 `hdr_fusion_crop.png`。
- 代价：**需要 2–3 帧同一场景**，静止场景可用；运动场景会有鬼影。4K 下实测单流约 11.6 fps，三帧融合后等效 ≤3.9 fps。
- 工程位置：可放进现有的 mini3a 架构（它已经有手动曝光控制 + 取帧循环），不碰内核。
- 偏色说明：上图偏绿是因为**没有跑 AWB**（3A 关闭），只用于验证动态范围，不代表最终色彩。

### 路线 B：启用传感器 DOL X2（2-lane HDR 表）

IMX415 的 HDR 是 DOL（Digital Overlap）：一帧内出长/短两次曝光，分两个 virtual channel 进 ISP，由 ISP 的 amerge 模块合成。

要做的事：
1. 照 4-lane 的 `imx415_hdr2_10bit_3864x2192_1485M_regs` 改出一张 2-lane 版本（lane 相关寄存器、HTS/VTS、MIPI 速率都要重算）；
2. 在 `supported_modes` 加条目（`hdr_mode = HDR_X2`，`vc[PAD0..3]` 按 DOL 的 VC 分配）；
3. IQ 增加/确认 `aec_calib` 的 HDR 段落（曝光比、长短帧策略），amerge 标定沿用；
4. 用 `v4l2-ctl --list-subdev-framesizes` 与抓帧确认两路 VC 都进得来，再让 rkaiq 跑 HDR。

- 风险：MIPI 带宽翻倍（4K10bit×2 在 2-lane 上未必够，可能只能跑到 1080p30 以下）；寄存器表要对着 datasheet 逐条核。
- 工作量：寄存器表 1 天 + 调试 1 天（不含 IQ 调优）。
- 前置条件：**先确认 2-lane 下 MIPI 速率上限**，否则做了也跑不到 4K。

### 路线 C：改 4-lane 走线 + 直接启用现成表

- SDK 里已有 4-lane 的完整 HDR 表，也带 `overlay/rk356x-csi0-imx415-4lane-overlay.dtbo`，理论上把 DTS 改成 `data-lanes = <1 2 3 4>` 再打开 `#if 0` 即可。
- 但本板 FPC/连接器是否 4-lane 连通需要硬件确认；改错会导致 CSI 完全不出图。
- 工作量：如果硬件支持，半天；不支持则不可行。

---

## 4. 建议的推进顺序

1. **现在**：把路线 A 做成一个可复用的组件（`hdr_fuse`），输入 2–3 帧 NV12 输出 1 帧；先在 RK3568 上量 CPU 开销，能接受就接到 mini3a 后面做一个"静态场景 HDR"演示。
2. **下一步**：先做**带宽核算**（4K/1080p 在 2-lane DOL X2 下是否可行）。核算通过再投入路线 B 的寄存器表。
3. **路线 C 暂缓**，除非能确认板子 4-lane 连通。

判断依据：A 的收益（高光 5.57%→0）已经能用；B 的收益是"运动场景也 HDR"，但成本和风险高一个量级。

---

## 5. 复现命令

```bash
# 0) 关键前提：实验期间必须没有 AE 在跑，否则手动曝光会被立刻覆盖
systemctl stop rkaiq_3A        # 本仓库 M4 已把它做成可 systemd 管理
pidof rkaiq_3A_server || echo none

# 1) 三档曝光各抓一帧（4K NV12）
for e in 60 300 1400; do
  v4l2-ctl -d /dev/v4l-subdev6 --set-ctrl exposure=$e
  sleep 0.4
  /root/grab /dev/video5 6 /root/hdr/e$e.nv12 3840 2160
done

# 2) 拉到 PC 上做融合（脚本：tools/hdr_fuse.py）
python hdr_fuse.py --in dim.nv12 mid.nv12 bright.nv12 --out fused.nv12
```

> 注意：`systemctl stop rkaiq_3A` 之后如果 ISP 已经出过流，重新起 3A 前建议先重启（见 docs/21 §9.6 的"rkaiq 被杀 → ISP 卡住"一条）。

---

## 6. 遗留问题 / 诚实清单

- 路线 A 的融合在 ARM 上的耗时**还没测**（PC 上 numpy 版本约 0.6 s/4K 帧，板端需重写并实测）。
- 融合对运动场景无效（鬼影），这是原理限制，不是实现问题。
- 路线 B 的 2-lane 带宽上限**未核算**，这是它成立与否的关键前提。
- 本预研没有动 IQ、没有动驱动，只做了取数验证；所有 HDR 相关代码路径仍保持出厂状态。
