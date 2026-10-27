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
