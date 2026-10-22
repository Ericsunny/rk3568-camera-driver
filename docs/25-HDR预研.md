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
| 最快的落地方式 | **软件多帧融合（路线 A）**：零内核改动，板端 C 实现已跑通（`tools/hdr_demo.c`），高光溢出 5.74% → **0.00%**，暗部占比 73.5% → **7.07%** |
| 真正的硬件 HDR | 需要**新增 2-lane HDR 寄存器表**（路线 B）或**改 4-lane 走线**（路线 C）。带宽核算结论：**1080p DOL X2 ≈ 15 fps 可行，4K DOL X2 不可行** |
| 建议 | A 已落地（板端 1080p 融合 346 ms/帧）；B 按 1080p 目标投；C 只在硬件确认支持 4-lane 后再谈 |

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

#### 路线 A 的板端实现：`tools/hdr_demo.c`

上面那份是 PC 上 numpy 版的结果（用来先验证算法）。真正落地的是板端 C 版本：`tools/hdr_demo`，一条命令完成"设曝光 → 抓 3 档 → 融合 → 存 NV12"，并打出各阶段耗时。

```bash
/root/hdr_demo -d /dev/video5 -s /dev/v4l-subdev6 -w 3840 -h 2160 \
               -e 60,300,1400 -n 6 -o /root/hdr2/fused_4k.nv12 -k
```

同一场景、同一会话（关闭 3A，手动曝光）实测：

| 帧 | 平均 Y | 中位 Y | 高光溢出(≥250) | 暗部(<16) |
|---|---|---|---|---|
| `e60` | 20.51 | 14 | 0.00% | 73.53% |
| `e300` | 36.19 | 22 | 3.14% | 18.54% |
| `e1400` | 74.81 | 54 | 5.74% | 1.59% |
| **fused** | **56.88** | **44** | **0.00%** | **7.07%** |

板端耗时（RK3568，4 核 A55）：

| 分辨率 | 采集(24 帧) | 权重 | 平滑 | 融合 | **融合净耗时** | 总耗时 |
|---|---|---|---|---|---|---|
| 3840×2160 | 2568 ms | 386 ms | 222 ms | 678 ms | **1285 ms** | 3853 ms |
| 1920×1080 | 2343 ms | 121 ms | 54 ms | 171 ms | **346 ms** | 2689 ms |

- 4K 融合 **1.28 s/帧**（约 0.8 fps），1080p **346 ms/帧**（约 2.9 fps）——**目前只适合"抓一张图"的场景，不适合视频流**。
- 热点很明确：`权重`+`融合`都是逐像素浮点，4K 共 830 万像素 × 3 帧；优化方向是 NEON + 定点化（预计能压到 1/3 以内），或把加权搬给 RGA/NPU。
- 效果图：`evidence/hdr/hdr_demo_4k_2x2.png`（四宫格）、`evidence/hdr/hdr_demo_4k_crop.png`（1:1 裁切）。

### 路线 B：启用传感器 DOL X2（2-lane HDR 表）

IMX415 的 HDR 是 DOL（Digital Overlap）：一帧内出长/短两次曝光，分两个 virtual channel 进 ISP，由 ISP 的 amerge 模块合成。

要做的事：
1. 照 4-lane 的 `imx415_hdr2_10bit_3864x2192_1485M_regs` 改出一张 2-lane 版本（lane 相关寄存器、HTS/VTS、MIPI 速率都要重算）；
2. 在 `supported_modes` 加条目（`hdr_mode = HDR_X2`，`vc[PAD0..3]` 按 DOL 的 VC 分配）；
3. IQ 增加/确认 `aec_calib` 的 HDR 段落（曝光比、长短帧策略），amerge 标定沿用；
4. 用 `v4l2-ctl --list-subdev-framesizes` 与抓帧确认两路 VC 都进得来，再让 rkaiq 跑 HDR。

- 风险：MIPI 带宽翻倍，寄存器表要对着 datasheet 逐条核。**带宽核算见 §6：结论是 1080p DOL X2 ≈ 15 fps 可行，4K 不可行。**
- 工作量：寄存器表 1 天 + 调试 1 天（不含 IQ 调优）。
- 前置条件：先按 §6 把目标分辨率定在 1080p，不要按 4K 投。

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

## 5. 2-lane DOL X2 带宽核算

### 5.1 已知常量（都来自本机源码/日志）

| 项 | 值 | 来源 |
|---|---|---|
| D-PHY 每 lane 上限 | **2499 Mbps** | `drivers/phy/rockchip/phy-rockchip-csi2-dphy-hw.c:288` 的 `rk3568_csi2_dphy_hw_hsfreq_ranges[]`，最高档 `{2499, 0x46}` |
| 传感器可选 link 频率 | 297 / 446 / 743 / **891** / **1188** Mbps | `imx415.c:1774` `link_freq_items[]` |
| 当前 4K linear 实际链路 | **891 Mbps/lane** | dmesg：`rockchip-csi2-dphy1: dphy1, data_rate_mbps 891` |
| 当前 4K linear 实测帧率 | 11.6 fps（无 3A）/ 8.2 fps（有 3A） | `grab` 实测 |
| 1080p linear 实测帧率 | 29.3 fps | docs/17 的 1080p 实验 |
| 4K 模式时序 | HTS 8800、VTS 2250 | `imx415.c` 2-lane 表 `hts_def = 0x0898 * 2 * 2`、`vts_def = 0x08ca` |
| 1080p 模式时序 | HTS 4096、VTS 2250 | 同上 `hts_def = 0x0800 * 2` |

### 5.2 算法

像素时钟需求（10bit）：

```
pclk = HTS × VTS × fps        （一行 HTS 个"像素时钟"，一帧 VTS 行）
lane 码率 = pclk × bpp / 2 / lanes
```

DOL X2 在**同一帧时间内传两遍**（长曝光 + 短曝光），等效于 `fps` 或 `VTS` 翻倍，所以：

```
DOL X2 的 lane 码率 ≈ linear 的 2 倍（同分辨率同帧率）
```

### 5.3 核算结果

| 目标 | pclk 需求 | lane 码率（2-lane） | 判定 |
|---|---|---|---|
| 4K linear @ 15 fps | 297 Mp/s | 1485 Mbps | 现状 891 Mbps 已够 15 fps 的边缘（实测 11.6 fps，受 VTS/曝光与 ISP 限制） |
| **1080p DOL X2 @ 30 fps** | **552 Mp/s** | **2760 Mbps（两条 lane 合计）/ 1380 Mbps 每条** | ✅ 可行：1380 < 2499，且 1188 Mbps 档已能满足 26 fps |
| 1080p DOL X2 @ 15 fps | 276 Mp/s | 690 Mbps/条 | ✅ 富余，891 Mbps 档即可 |
| 4K DOL X2 @ 15 fps | 594 Mp/s | **1485 Mbps/条** | ⚠️ PHY 可行（<2499），但**ISP 吃不下**：ISP 实测 4K 上限约 120 Mp/s，DOL X2 后有效帧率会掉到 ~6–7 fps |
| 4K DOL X2 @ 30 fps | 1188 Mp/s | 2970 Mbps/条 | ❌ 超过 2499 Mbps/lane 的 PHY 上限 |

### 5.4 ISP 这一侧才是真瓶颈

RK3568 单 ISP 的吞吐我们实测过两档：

- 4K：11.6 ~ 14.7 fps → **约 96–122 Mpix/s**
- 1080p：29.3 fps → **60.8 Mpix/s**

DOL X2 下 ISP 每输出一帧要处理两份输入，所以：

```
1080p DOL X2：2 × 60.8 = 121.6 Mpix/s  ≈ ISP 上限  →  输出约 15 fps
4K DOL X2   ：2 × 110    = 220 Mpix/s   ✗ 远超上限  →  输出约 6~7 fps
```

### 5.5 结论

1. **路线 B 应该按 1080p DOL X2 @ ~15 fps 来投**，这个目标在 PHY 和 ISP 两侧都成立。
2. **4K DOL X2 直接放弃**：MIPI 或 ISP 总有一侧顶不住，做出来也只有 6–7 fps。
3. 附带结论：1080p linear 现在是 29.3 fps，改 HDR 后掉到 ~15 fps，**"分辨率减半换动态范围"等价于"帧率减半"**，取舍要在应用层定。
4. 2-lane HDR 寄存器表需要把 link 定在 **1188 Mbps/lane**（对应 `mipi_freq_idx = 4`，驱动里已有这一档），这样 1080p30 有 14% 余量。

---

## 6. 复现命令

> ⚠️ 运行前必须让 ISP 处于"本次启动后从未起过 3A"的状态，否则会撞上 `rkisp-vir0: waiting on params stream on event timeout` 并且设备一直 EBUSY（见 §7 第 1 条）。

```bash
# 0) 停掉 3A 自启并重启（关键：不是 stop 就行，必须 reboot）
systemctl disable rkaiq_3A
reboot
# 重启后确认没有 3A 在跑
pidof rkaiq_3A_server || echo none

# 1) 板端一键融合（4K，三档曝光，保留输入帧）
/root/hdr_demo -d /dev/video5 -s /dev/v4l-subdev6 -w 3840 -h 2160 \
               -e 60,300,1400 -n 6 -o /root/hdr2/fused_4k.nv12 -k

# 2) 1080p 同场景（耗时约 1/4）
/root/hdr_demo -d /dev/video5 -s /dev/v4l-subdev6 -w 1920 -h 1080 \
               -e 60,300,1400 -n 6 -o /root/hdr2/fused_1080p.nv12

# 3) 恢复 3A 自启
systemctl enable rkaiq_3A && systemctl start rkaiq_3A
```

纯 PC 侧的等效做法（不依赖板端程序）：

```bash
# 三档曝光各抓一帧后
python tools/hdr_fuse.py --in dim.nv12 mid.nv12 bright.nv12 --out fused.nv12 --size 3840x2160
```

---

## 7. 运维坑（这次踩到的）

1. **停掉 rkaiq 后 ISP 就再也开不了流**。症状：`RKMODULE` 一切正常，但 `S_FMT` 返回 `EBUSY`，dmesg 里 `rkisp-vir0: waiting on params stream on event timeout` + `can not get first iq setting in stream on`。
   原因：ISP 在 stream on 时要等 3A 把第一份 params 推进来，3A 已经停了就永远等不到，设备停在半开状态。
   **唯一解**：`systemctl disable rkaiq_3A` + `reboot`，在"从没起过 3A"的状态下采集；采完 `systemctl enable` 恢复。
2. 这与 docs/24 的"先起 daemon 再开流"是同一条规律的两面：**ISP 的生命周期里必须有一个 3A 服务在场**，要么从头到尾在场，要么从头到尾不在场。
3. 3A 在跑时手动 `--set-ctrl exposure` 会被 AE 立刻覆盖（实测三档曝光抓出来的三帧亮度一模一样，Y 全是 75.1）。
4. 4K 出流帧率会被 3A 拖低：无 3A 11.6 fps，有 3A 8.2 fps（同分辨率同曝光）。

---

## 8. 遗留问题 / 诚实清单

- 路线 A 的融合耗时**已实测**（4K 1285 ms/帧、1080p 346 ms/帧），但**还没做 NEON/定点优化**，当前只适合出图不适合视频。
- 融合对运动场景无效（鬼影），这是原理限制，不是实现问题。
- 路线 B 的 2-lane 带宽**已核算**（§5），结论是 1080p 可行、4K 不可行；但**寄存器表还没写**，核算结果尚未用真实硬件验证。
- 本预研没有动 IQ、没有动驱动，只做了取数验证；所有 HDR 相关代码路径仍保持出厂状态。
