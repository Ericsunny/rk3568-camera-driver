# 2026-10-27 HDR 前置:线性基线(#52)

## 1. link 频率口径核对(先做的第一件事)

`imx415.c` 的 `link_freq_items[]` 是 **link 时钟频率(Hz)**,dphy 侧按
`data_rate_mbps = item × 2 / 1e6` 换算(`phy-rockchip-csi2-dphy.c:93`):

| `mipi_freq_idx` | link 时钟 | 实际 data rate |
|---|---|---|
| 0 | 297 MHz | 594 Mbps |
| **1** | **446 MHz** | **891 Mbps** |
| 2 | 743 MHz | 1486 Mbps |
| 3 | 891 MHz | 1782 Mbps |
| 4 | 1188 MHz | 2376 Mbps |

**结论:`mipi_freq_idx = 1` 与 docs/26 的"891 Mbps/lane"不矛盾**——891 指 data rate,对应 446 MHz 时钟。
docs/25 原表把五档写成 "Mbps"、第 175 行写 "1188 Mbps/lane(idx=4)" 属**单位错误**(应为 2376 Mbps),已在 docs/25 勘误并新增换算表。

## 2. 4K 线性基线(有效数据)

3A 常驻(active),`st_burn -d /dev/video5 -w 3840 -h 2160 -t 30`:

```
LONG secs=30 frames=444 fps=14.73 gaps=0 loss_pct=0.000
     min_ms=60.9 med_ms=66.7 max_ms=72.2 rc=0
```

即 **4K@15fps 模式稳定 14.73 fps、零丢帧、无 gap**;dmesg 无 timeout。

## 3. 1080p 基线:未成立(现场记录)

- 直接 `st_burn -w 1920 -h 1080` 得到 **与 4K 完全相同的 14.74 fps/444 帧**——
  说明只改 video5 输出尺寸**不会切 sensor 模式**,sensor 仍跑 3864×2192@15;
- 用 `media-ctl -d /dev/media1 --set-v4l2 '"m00_b_imx415 1-001a":0[fmt:SGBRG10_1X10/1944x1097]"` 单点设置后,
  拓扑出现**混合状态**(sensor 部分实体 1944×1096、上游仍 3864×2192/YUYV 3840×2160),
  采 30 s 得 **0 帧 / loss 100%**,dmesg 出现 1 次 timeout。

**结论**:切 1080p 必须把整条 ISP 管线的格式一起改(至少 sensor → rkisp-isp-subdev sink/source → video5),
不能只设 sensor;单点设置会留下半配置状态(0 帧)。本次未继续在半配置状态上折腾,记录后重启。

## 4. 下一步(等重启)

1. 用"整条管线"的方式设 1080p(sensor 1944×1096 → isp-subdev pad0 → pad2 NV12 1920×1080 → video5),
   再跑 `st_burn 1080p 30s`,补齐 HDR 所需的 1080p 基线;
2. 之后才进 op_mode {5,8,9} 矩阵(时间盒)。

## 5. 1080p 管线切换的完整步骤(本轮摸清,但 3A 在跑时仍 0 帧)

按顺序设四件(crop 是关键,漏了它 pad2 会拒绝改尺寸):

```bash
media-ctl -d /dev/media1 --set-v4l2 '"m00_b_imx415 1-001a":0[fmt:SGBRG10_1X10/1944x1096]'
media-ctl -d /dev/media1 --set-v4l2 '"rkisp-csi-subdev":1[fmt:SGBRG10_1X10/1944x1096]'
media-ctl -d /dev/media1 --set-v4l2 '"rkisp-isp-subdev":0[fmt:SGBRG10_1X10/1944x1096]'
media-ctl -d /dev/media1 --set-v4l2 '"rkisp-isp-subdev":0[crop:(0,0)/1944x1096]'   # ★ 必须先修 crop
media-ctl -d /dev/media1 --set-v4l2 '"rkisp-isp-subdev":2[fmt:YUYV8_2X8/1920x1080]'
```

设完后拓扑一致:`1944×1096 → crop(12,8)/1920×1080 → pad2 YUYV 1920×1080`。

**但在 3A 运行中执行这套后,采集仍然 0 帧 + 1 次 timeout**——rkaiq 的 ISP params 是按 4K 配置的,
格式被换掉后 params 对不上;而按 docs/29 §10(A4′/E1)又不能"停 3A 继续采"。

⇒ **1080p 基线必须在 3A 从未启动的环境里做**(`systemctl disable rkaiq_3A` + 重启,即 A4/E2 路径),
这既是生命周期规则的要求,也是唯一不 wedge 的路径。已停用 3A,等重启后补测。

## 6. 1080p 基线结论:被厂商 1080p 模式挡住(重要)

在 **3A 从未启动**(`disabled/inactive`)的干净环境下,按 §5 设好整条管线(拓扑一致:
`1944×1096 → crop(12,8)/1920×1080 → pad2 YUYV 1920×1080`)后:

- `st_burn -w 1920 -h 1080 -t 30` → **0 帧 / loss 100%**,dmesg `rkisp_stream_stop id:0 timeout`;
- sensor 侧:`s_stream: 1. 1944x1096, hdr: 0, bpp: 10` → **`write reg array error` ×2** →
  `write reg array, sleep 30ms` 重试 → 随后 `set vblank 0x482 vts 2250`、`set exposure`、`set analog gain`;
- **CSI/MIPI 侧无任何 error/overflow/fifo 日志**。

判读:sensor 的 1080p 寄存器表(驱动内 `imx415_linear_10bit_1920x1080_891M_regs_2lane`,模式 1944×1096)
在本板**没能完成写入/没真正切到 1080p 输出**,于是 ISP 等不到 1944 宽的帧 → 0 帧。
4K 模式(3864×2192)同路径正常,排除 I2C 硬件问题。

**对 HDR 的影响(关键)**:`docs/26` 的 HDR 派生表以这张 1080p 表为基底、尺寸 1944×1096。
**基底模式本身在本板不出帧,HDR 的 1080p 前提不成立** —— HDR 时间盒的第一步不应是 op_mode 矩阵,
而是先把"1080p 线性模式能出帧"这件事解决(查厂商表/补表)。

## 7. 当前状态

- 1080p 尝试后 ISP 再次 wedge(`rkisp_stream_stop timeout`),需重启恢复;
- 已 `systemctl enable rkaiq_3A` 恢复默认(重启后 3A 自动 active)。

## 8. E0 探针结果(2026-10-27,定点到 sensor MIPI 输出)

按用户 E0 规格做定位(未改内核,用驱动自带日志 + 中断计数 + 静态表对比):

| 假设 | 判定 | 依据 |
|---|---|---|
| 3A 生命周期干扰 | **排除** | 3A `disabled/inactive`(本 boot 从未启动)下同样 0 帧 |
| `write reg array error` = 表写失败 | **排除** | 该日志来自 `imx415_write_array()` 的 `if (!regs)`,而 `global_reg_list` 在 2-lane 模式里本就是 NULL;`reg_list` 随后照常写(`imx415.c:2696`) |
| 模式选择错误 | **排除** | dmesg:`imx415_find_best_fit: cur_best_fit(1)`、`set fmt: cur_mode: 1944x1096, hdr: 0, bpp: 10`、`mipi_freq_idx(1)` |
| DPHY 速率 | **排除** | `rockchip-csi2-dphy1: dphy1, data_rate_mbps 891`(与 idx=1 一致) |
| sensor 是否输出数据 | **确认没有** | 流式期间 `rockchip-mipi-csi2` 两路 IRQ 计数 **增量 0**;`s_stream: 1. 1944x1096` 已进流 |

**结论**:sensor 进入 1080p 模式、dphy 配好,但 MIPI 上无数据 → 问题在 sensor 的 1080p 输出通路(寄存器表/时序)。

静态表对比(1080p 141 项 vs 4K 131 项):
- 仅 4K 有:`0x3051 = 0x07`(SHR0 高字节,驱动随后会按曝光重写,疑无关);
- 仅 1080p 有:`0x301c=0x04`、`0x3020/21/22=0x01`、`0x3024/25=0x08ca`(VTS 2250,设计如此)、`0x3034=0x05`、`0x30d9/30da=0x02/0x01`、`0x4023=0x00`。

## 9. 下一步(E0 继续,需重启后做)

在**流式期间**用 `i2cget` 回读上述寄存器(以及 0x3000/0x3001 等模式/待机位),与 4K 模式的同一组值对照,
找出"1080p 模式下取值异常"的那一条;若一天内无法定位到具体寄存器,按时间盒转方案 2(暂停 HDR,记为独立驱动任务)。

## 10. E0 结论(2026-10-27):定位到一处真实缺陷,但不充分

**发现**:`imx415_linear_10bit_1920x1080_891M_regs_2lane` 表里 `{0x301C, 0x04}` 这一条**被注释掉了**
(同一文件里 720p 的两张表都正常写它)。流式回读证实:修复前 0x301C=0x00,其余条目(0x3020/21/22、VTS、0x30d9/da)都正确生效。

**修复并验证(#53)**:取消注释后重建内核写入 /boot,流式回读 **0x301C=0x04 已生效**,
但 **1080p 仍 0 帧、`rockchip-mipi-csi2` 中断增量仍为 0** → 这条是必要不充分。
剩余唯一异常:`0x3034`(表写 0x05,实读 0x00,疑只读/自清零),继续需要 datasheet 对照。

**处置**:按时间盒纪律,停止继续挖;板子已回退到已验证的 #52(`04672f71`),
`/root/Image.p53`(`811f6a32`)保留供复现。实验补丁见 `driver/patches/imx415-1080p-0x301c-experimental.patch`
(标记 EXPERIMENTAL / NOT validated)。

**结论(收窄口径)**:当前 BSP 的这张 2-lane 1080p 寄存器表**未打通**——已确认表内 `0x301C` 被注释是一个真实缺陷,
但取消注释后仍 0 帧,说明它不是充分原因;`0x3034` 回读异常(写 0x05 读 0x00,可能是只读/自清零,也可能是缺失初始化)尚未解释,
**根因未完全闭合**。HDR 时间盒暂停待 1080p bring-up;恢复 HDR 时的第一项工作是对照 720p/4K 正常模式的 `0x3034` 回读,
确认它是否本来就自清零,再决定是否需要 datasheet。不写"IMX415 1080p 永久不可用"。
