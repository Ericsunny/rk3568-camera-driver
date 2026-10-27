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
