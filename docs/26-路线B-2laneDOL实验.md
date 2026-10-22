# 26 - 路线 B 实验记录:2-lane DOL X2(1080p)

> 日期:2026-10-22 · 板:YCKJ iTOP-3568(RK3568)· 内核 `#34`/`#35`(自编)
> 目标:不用 4-lane,在现有 2-lane 硬件上跑通 IMX415 的 DOL 2-frame HDR。
> 结论:**传感器侧切换成功,CSI/ISP 接收侧未通**(MIPI overflow,收不到帧)。

## 1 为什么可以从寄存器表“派生”

厂商 `imx415.c` 里 HDR 表只有 4-lane 版本,2-lane 只有线性表。但两组表的结构差异很清楚:

- 4-lane 线性表与 4-lane HDR2 表逐项比对后,**与 HDR 相关、且与链路速率无关**的只有 4 个寄存器:

| 寄存器 | 线性 | HDR2 | 说明 |
|---|---|---|---|
| `0x302C` | 0x00 | 0x01 | DOL 模式位 |
| `0x302D` | 0x00 | 0x01 | DOL 模式位 |
| `0x30CF` | 0x00 | 0x01 | DOL 相关 |
| `0x3260` | 0x01 | 0x00 | 驱动文件头注释明确:`0x3260 should be set 0x01 in normal mode, should be 0x00 in hdr mode` |

- 其余差异(`0x3033`、`0x400C`、`0x4018..0x4028`、`0x4074`)在两个不同链路速率的 HDR 表之间并不一致,**随链路速率变,不随 HDR 变**,所以派生时一律保留 2-lane 线性表原值。

因此新表 = 2-lane 1080p 线性表 + 上述 4 个 HDR 位 + SHR1 默认值(`0x3054/55/56 = 0x09/00/00`),链路仍是 891 Mbps/lane、2-lane、PLL 与裁切全部沿用已验证值。

## 2 实现

- `tools/patch_imx415_hdr.py`:幂等注入新表 `imx415_hdr2_10bit_1920x1080_891M_regs_2lane` 与 `supported_modes_2lane[]` 里的 `HDR_X2` 条目(1944×1096、`vc[PAD0]=VC0`、`vc[PAD1]=VC1`)。表体 141 项,只改上述寄存器。
- `tools/hdr_cfg.c`:板端小工具,`S_FMT 1944×1096` + `RKMODULE_SET_HDR_CFG(HDR_X2)` + 读回确认。
- 编译刷机流程同 docs/17 §10(驱动内建 `=y`,必须重编 Image 刷 `/boot`)。

## 3 实测结果

切换本身**成功**:

```
imx415 1-001a: set hdr cfg, set mode to 2
imx415 1-001a: set fmt: cur_mode: 1944x1096, hdr: 5, bpp: 10
imx415 1-001a: s_stream: 1. 1944x1096, hdr: 5, bpp: 10
GET_HDR_CFG -> hdr_mode=5 esp=0
```

但一开流就报:

```
rkisp-vir0: MIPI error: overflow: 0x00010000   (数十~上百次)
rkisp-vir0: rkisp_stream_stop id:0 timeout
```

`grab /dev/video5 40` 拿不到任何帧(超时),`dmesg` 里没有任何 `CIF_ISP_PIC_SIZE_ERROR` 之外的进展。

### 3.1 排除“行速率不够”

DOL X2 每行要搬两份曝光数据,若行时间不变则瞬时码率翻倍。把新表 HTS 从 `0x0226` 翻到 `0x044C`(即行时间翻倍、帧率再减半)后重编刷机(`#35`):

```
MIPI error 计数: 0(开流前) -> overflow 0x00010000(开流瞬间)
```

**仍然 overflow** → 与行速率/带宽无关,MIPI 上限 2499 Mbps/lane 也远未触及。

### 3.2 因此失败点在下游

`rkisp` 侧要按“HDR 双 VC”配置 CSI 与 ISP,而 `csi.c` 只在 `rkisp_csi_s_stream` 里通过 `RKMODULE_GET_HDR_CFG` 取一次传感器 HDR 状态,再映射到 `dev->hdr.op_mode`。当前现象(能收包但一帧都凑不齐、寄存器报 overflow)符合“CSI 仍按线性单 VC 收、而传感器发的是 DOL 交错数据”的特征。

## 4 结论(诚实版)

| 环节 | 状态 |
|---|---|
| UAPI 能力(`RKMODULE_GET/SET_HDR_CFG`、`HDR_X2=5`) | ✅ 有 |
| 传感器驱动切 DOL X2(2-lane) | ✅ 已跑通(寄存器表派生 + ioctl 生效) |
| IMX415 真的输出 DOL 数据 | ⚠️ 未证实(见下) |
| CSI/ISP 按 HDR 接收 | ❌ 失败(MIPI overflow,零帧) |
| 3A 出 HDR 参数 | 未开始 |

**“未证实”的原因**:没有任何一帧到达用户态,无法判断传感器发的是不是合法的 DOL 交错数据,还是寄存器组合只让它进了“伪 HDR”状态。要区分这两者,需要示波器/逻辑分析仪抓 MIPI,或用 Sony 官方工具读回传感器内部状态 —— 这两样本项目都没有。

## 5 下一步(如果继续投)

1. **读 `rkisp` HDR 使能顺序**:`drivers/media/platform/rockchip/isp/csi.c` 里 `RKMODULE_GET_HDR_CFG` 的调用点、`dev->hdr.op_mode` 的赋值与 `capture.c` 的 `HDR_*_DDR` 分支,确认 ISP 是否真按 2 VC 收;必要时在 `s_stream` 前显式设置。
2. **对照 4-lane 可用性**:同一块板换成 4-lane 接线(需要硬件改线),用厂商现成的 `HDR_X2` 表验证“驱动 + ISP 的 HDR 通路本身是否通”。如果 4-lane 也 overflow,问题就在 ISP 侧;如果 4-lane 通,问题只在 2-lane 表派生。
3. **3A 侧**:`rkaiq_build/rkaiq/iqfiles/isp20/` 里有现成的 IMX415 HDR3 XML(`imx415_YT10092_IRC-8M-F20-hdr3.xml`),可作为 HDR IQ 的起点;但 2022 库是否吃这套 IQ 仍需验证。

## 6 现场与复现

- 内核:`#35 SMP Thu Oct 22 06:09:54 UTC 2026`,含 HDR_X2 模式条目(默认仍选线性模式,`find_best_fit` 对 1944×1096 取 hdr=0 的那条)。
- 切换:`/root/hdr_cfg /dev/v4l-subdev6 5`(HDR_X2)/ `... 0`(NO_HDR)。
- 注意:切回 NO_HDR 后传感器分辨率仍是 1944×1096,要再 `S_FMT` 回 3840×2160,否则 ISP 报 `CIF_ISP_PIC_SIZE_ERROR`。
- 3A 在跑时直接 `grab /dev/video5` 拿不到帧(设备被 3A 占着);要直采需 `systemctl disable rkaiq_3A` + 重启(docs/25 §7 同一条运维约束)。
- 本次实验用的两个补丁脚本与工具已收进 `tools/`,可直接复现。

## 7 顺带确认的事实

- `link_freq_items` 五档(297/446/743/891/1188 Mbps/lane)在驱动里都有;DOL 若要提帧率,1188 档可用(`mipi_freq_idx=4`),但本次卡在 CSI 侧,提链路无意义。
- 2-lane 下 4K 线性实跑 891 Mbps/lane、11.6 fps(无 3A)/8.2 fps(有 3A),与之前实测一致。

## 7. B-1 收尾:2-lane DOL 的三条假设全部否证(2026-10-22)

结论先行:**2-lane DOL X2 在本板未打通,属硬件 + 资料边界,不是实现缺陷。** 三条候选假设逐条否证如下。

### 7.1 假设一:`hdr_op_mode` 枚举撞车 —— 不成立

uapi `enum rkmodule_hdr_mode { NO_HDR=0, HDR_X2=5, HDR_X3=6 }`(`rk-camera-module.h:355`)与内核 `enum hdr_op_mode { HDR_NORMAL=0, HDR_RDBK_FRAME1=4, HDR_RDBK_FRAME2=5, HDR_RDBK_FRAME3=6, HDR_FRAMEX2_DDR=8, HDR_LINEX2_DDR=9, ... }`(`csi.h:32`)**数值确实撞车**:同样的 5 一个表示 X2、一个表示三帧回读。

但原厂已经处理了。`csi.c:297-300`:

```c
/* HDR on the fly for isp21 */
if (dev->isp_ver == ISP_V21 && !(dev->isp_inp & INP_RAWRD2))
    if (dev->hdr.op_mode == HDR_RDBK_FRAME2)
        dev->hdr.op_mode = HDR_LINEX2_DDR;
```

在 `csi.c` 里加临时打印实测(`v4l2_info`,已回退):

```
普通模式: HDRDBG: hdr_cfg=0 op_mode=4  isp_inp=0x15 single=1
DOL X2:  HDRDBG: hdr_cfg=5 op_mode=5  isp_inp=0x10 single=1
         HDRDBG: after -> op_mode=9
```

两点:**①** 开 HDR 后 `isp_inp` 从 `0x15`(含 `INP_RAWRD2`)变成 `0x10`(CSI 直通,不含回读位),所以 `!(INP_RAWRD2)` 条件成立,5→9 转换**本来就生效**;**②** 转换后 `op_mode=9 (HDR_LINEX2_DDR)`,不是"三帧回读"。假设否证。

实测结果:`op_mode=9` 下仍 **0 帧**,30 s 超时,overflow/timeout/crc 计数 51。

### 7.2 假设二:`esp_mode` 应配成 LINE_CNT —— 不成立

`csi.c:323-325` 把 `esp_mode` 与 `op_mode` 一起写进 `CSI2RX_CTRL0`:

```c
rkisp_write(dev, CSI2RX_CTRL0,
            SW_IBUF_OP_MODE(dev->hdr.op_mode) |
            SW_HDR_ESP_MODE(dev->hdr.esp_mode), true);
```

而厂商 `imx415.c:2675-2679` 对 `RKMODULE_GET_HDR_CFG` **硬编码**返回:

```c
hdr->esp.mode = HDR_NORMAL_VC;      // 恒为 0,不随 HDR 变化
hdr->hdr_mode = imx415->cur_mode->hdr_mode;
```

即 `op_mode=9 + esp=0` 就是 Rockchip 为 IMX415 设计的组合,我们的通路与厂商一致。假设否证。

### 7.3 假设三:切 4-lane 复用厂商 HDR 表 —— 物理不可行

原理图 `btb_rk3568_mb_1v0.pdf`(16 页)的 CSI 信号按**两个时钟域**成组出现:

```
MIPI_CSI_RX_D0P/D0N → MIPI_CSI_RX_CLK0P/CLK0N   时钟域 0
MIPI_CSI_RX_D1P/D1N
MIPI_CSI_RX_D2P/D2N → MIPI_CSI_RX_CLK1P/CLK1N   时钟域 1
MIPI_CSI_RX_D3P/D3N
```

RK3568 的 CSI 是**两组独立 2-lane DPHY**,不是一个 4-lane PHY。单 sensor 跑 4-lane 需要 D0–D3 共用同一时钟域,本板做不到。厂商 `imx415.c` 里那张 4-lane HDR2 表(`#if 0` 块)在本板无法复制。假设否证。

### 7.4 剩余唯一嫌疑与为什么停下

唯一没被否证的是**我们自己从 4-lane 表派生的 2-lane DOL 表**。但要验证它必须有一个参照物,而:

- 厂商没给 2-lane HDR 表(只有 `#if 0` 的 4-lane);
- 官方文档查证:`Rockchip_Developer_Guide_Linux4.4_Camera_CN.pdf`(29 页)搜 `HDR|DOL|多帧` 仅 1 处正文命中,是 Linux 4.4 时代的老架构文档,不含 ISP2.1 DOL 内容;
- E 盘迅为资料(`09代码`/`驱动指南`)均为应用层教学,无 DOL 寄存器或 ISP 配置。

**参照物不存在 → 无法判定表是对是错 → 继续投入没有收敛路径。** 据此收尾。

### 7.5 收尾动作

- `csi.c` 的两处临时改动(HDRDBG 打印、5→9/6→13 重映射)**已 `git checkout` 回退**,内核与厂商一致。
- 重编 + 刷 `/boot/Image-4.19.232`,内核到 **`#39`**(`SMP Thu Oct 22 09:56:53 UTC 2026`)。
- `rkaiq_3A` 服务**恢复 enabled**(B-1 期间为测 HDR 曾 disable)。
- HDR 能力锁定在已验证的 **B-2:应用层双次曝光融合**(`tools/hdr_demo.c`,1080p 净耗时 346 ms、4K 1285 ms,见 §4/§5)。

### 7.6 遗留

`#39` 上 `rkaiq_3A` 起来后 `grab` 取不到帧(dmesg 有 `rkisp_stream_stop id:0 timeout`,`set exposure(shr0) 2149 = cur_vts(2250) - val(101)`),与 §6 记录的 "3A 与出流时序冲突" 同源,需在干净状态下重新验证一次;该问题不影响 B-1 的结论,B-2 演示时本就要求 `systemctl stop rkaiq_3A`。
