# ov8858_sm —— 从 datasheet 写的 OV8858 V4L2 subdev 驱动

OmniVision **OV8858**(8MP,2-lane MIPI CSI-2,R2A 版本)的裸写 sensor 驱动,跑在 RK3568 + Linux 4.19.232 上,接 `rkcif`(V4L2 CIF)链路。

- 驱动本体 **666 行**(`ov8858_sm.c`)+ 寄存器表 **449 行**(`ov8858_sm_regs.h`)
- **编进内核**(`CONFIG_VIDEO_OV8858_SM=y`),不是模块 —— 4.19 的 async 是自底向上链式,sensor 晚加载永远配不上已经 probe 完的桥
- 支持两种模式:**1632×1224**(2×2 binning,厂商表)和 **1920×1080**(自推,全阵列读出+裁窗)

## 实测结果

| 模式 | HTS / VTS | 帧率 | 帧结构(有效行) | 状态 |
|---|---|---|---|---|
| 1632×1224 | 1928 / 1244 | **29.3 fps** | **1224 / 1224** | ✅ |
| 1920×1080 | 1940 / 1236 | **29.3 fps** | **1080 / 1080** | ✅ |

链路:`ov8858-sm → rockchip-csi2-dphy2 → rockchip-mipi-csi2 → stream_cif_mipi_id0 (/dev/video0)`,全部 ENABLED。

## 与厂商驱动的寄存器对照

| 表 | 厂商 | 我们 | 差异 |
|---|---|---|---|
| global(2-lane / R2A) | 311 条 | 311 条 | **0** |
| 1632×1224 mode | 30 条 | 30 条 | **0** |
| 1920×1080 | 厂商无此档 | 43 条 | 厂商全分辨率表骨架 + **13 改值 + 13 新增** |

完整对照见 [docs/17-L1-B对照报告-自写驱动vs厂商.md](../../docs/17-L1-B对照报告-自写驱动vs厂商.md)。

## 编译

```bash
# 1) 放进内核树
cp ov8858_sm.c ov8858_sm_regs.h <kernel>/drivers/media/i2c/

# 2) Kconfig 加
config VIDEO_OV8858_SM
	bool "OmniVision OV8858 SM sensor support"
	depends on I2C && VIDEO_V4L2_SUBDEV_API
	help
	  OmniVision OV8858 sensor driver (hand-written from datasheet).

# 3) Makefile 加
obj-$(CONFIG_VIDEO_OV8858_SM) += ov8858_sm.o

# 4) .config 里打开(必须是 y)
CONFIG_VIDEO_OV8858_SM=y

# 5) 编 Image
make ARCH=arm64 CROSS_COMPILE=<toolchain>bin/aarch64-rockchip-linux-gnu- -j6 Image
```

设备树节点(板级):

```dts
&cam1_ov8858 {
	pwdn-gpios  = <&gpio1 RK_PB1 GPIO_ACTIVE_HIGH>;
	reset-gpios = <&gpio1 RK_PB2 GPIO_ACTIVE_HIGH>;
	status = "okay";
	compatible = "ovti,ov8858-sm";
};
```

## 抓帧

```bash
# 1632x1224
media-ctl -d /dev/media0 --set-v4l2 '"ov8858-sm 5-0036":0[fmt:SBGGR10_1X10/1632x1224]'
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1632,height=1224,pixelformat=BG10
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=/tmp/a.raw

# 1920x1080(必须先设 sensor 子设备,否则 rkcif 会把宽夹回 1632)
media-ctl -d /dev/media0 --set-v4l2 '"ov8858-sm 5-0036":0[fmt:SBGGR10_1X10/1920x1080]'
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=BG10
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=/tmp/b.raw
```

**数据是 compact RAW10**(每 5 字节解 4 个 10bit 像素,stride 256 对齐):

```python
import numpy as np
BPL,H,W = 2048,1224,1632        # 1080p: 2560,1080,1920
b = np.fromfile('a.raw', dtype=np.uint8).reshape(-1, BPL)[:H, :W*10//8]
print("空行:", int(((b!=0).sum(axis=1)==0).sum()), "/", H)   # 应为 0
g = b.reshape(H, -1, 5).astype(np.uint16)
p = np.stack([(g[:,:,0]<<2)|(g[:,:,4]&3),  (g[:,:,1]<<2)|((g[:,:,4]>>2)&3),
              (g[:,:,2]<<2)|((g[:,:,4]>>4)&3), (g[:,:,3]<<2)|((g[:,:,4]>>6)&3)], 2)
```

## 踩过的三个坑(都写进了代码注释)

1. **`.video.g_mbus_config` 不实现 → `/dev/video0` 打不开(errno 515)**  
   内核 `rkcif_update_sensor_info()` 容忍了 `-ENOIOCTLCMD` 却忘了把 `ret` 清零,于是把 -515 当自己的返回值抛出去。这个 op 在 `v4l2_subdev_video_ops` 里,不在 `pad_ops`。
2. **probe 上电后没断电 → 偶发整帧垃圾**  
   clk/regulator 引用计数变成 2,关流只关一次,传感器两次开流之间不会真正复位。修法:`powered` 标志把上电/断电做成幂等,probe 读完 ID 主动断电。
3. **1080p 帧缺行(24% 的行全空)→ BLC anchor 越界**  
   `0x4020~0x4027` 的 ANCHOR 是以输出像素为单位;厂商两档内部都读满 3264 列所以从不越界,我们真裁到 1920 就必须按 `1920/3264` 重算。修完空行 179 → 0。

## 已知问题

- 1080p 下每帧打一条 `rockchip-mipi-csi2: ERR1: error matching frame start with frame end`。来自 Synopsys DW CSI-2 主控的 `err_bndry_match` 字段;该 SDK 在 CSI-2 路径上 `MSK1=0`(全开),只有 DSI-RX 才屏蔽这一位。CIF 侧 `all err count: 0`、`frame dma end` 满帧,数据通路无影响。
- 只验证 R2A + 2-lane 这一种组合。
