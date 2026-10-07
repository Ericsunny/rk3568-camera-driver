# RK3568 MIPI 相机子系统 Bring-up 学习项目

在 **YC_RK3568**(Rockchip RK3568,kernel 4.19.232)开发板上,从零完成 MIPI CSI-2 相机子系统的完整 bring-up:传感器点亮 → ISP 出图 → 裸写 sensor 驱动 → ISP/3A 调优 → 异构双摄 → 电源管理。

- **传感器**:Sony IMX415(4K Starvis,2-lane MIPI,野火模组 EBF410358)+ OmniVision OV8858(计划中,datasheet 裸写驱动练习)
- **转接**:野火"树莓派摄像头转鲁班猫"转接板(模组 24P → 板卡 15P FPC)
- **主机**:SDK 内核源码交叉编译(VM Ubuntu 20.04 + gcc-linaro 6.3.1),netboot(tftp+booti)部署 dtb

## 硬件链路

```
IMX415 模组(24P 0.5mm) → 野火转接板 → 15P 1.0mm FPC → 板卡 J10(CAM0, I2C1) / J11(CAM1, I2C5)
                                              ↓
              csi2-dphy1(split mode, lane0/1) → RKCSI → RKISP → /dev/video0 (mainpath, NV12)
```

两个 15P 座引脚序完全相同,唯一差异是背后的 I2C 控制器与 D-PHY 实例——**物理口与总线号必须先核对再调试**(血的教训,见复盘文档)。

## 里程碑

| 阶段 | 内容 | 状态 |
|------|------|------|
| L1-A Phase 1 | 设备树使能 i2c1 + imx415@1a,dtb 编译部署(netboot) | ✅ |
| L1-A Phase 2 | sensor probe(`Detected imx415 id 0000e0`)+ media 管线端到端挂载 | ✅ 2026-10-07 |
| L1-A Phase 3 | mainpath 第一帧 NV12 + RAW10 对照 | 🚧 |
| L1-B | 按 OV8858 datasheet 裸写 V4L2 subdev 驱动(不抄厂商代码) | 排队 |
| L2 | rkaiq + IQ 文件出图,直通 vs ISP 对比 | 排队(IQ 文件已从出厂 rootfs 收割) |
| L3 | 异构双摄(dphy1 + dphy2 split 并行) | 排队 |

## 文档

- [docs/09-L1A-I2C调试复盘](docs/09-L1A-I2C调试复盘-物理口与总线映射.md) —— 一次"现象全指向硬件供电、真凶是物理接口假设"的完整排障实录:三次误判修正链、物理口↔总线映射表、bring-up 七条纪律、模组/转接板原理图事实库。
- [docs/10-L1A-Phase3-第一帧任务书](docs/10-L1A-Phase3-第一帧任务书.md) —— V4L2 用户态抓帧实操(NV12/RAW10 双路径 + 预测题 + 验收标准)。

## 环境

- 板卡串口 1500000 8N1(CH340),SSH 静态 IP;内核/设备树在 VM 交叉编译,tftp netboot 到板
- 调试三件套:`media-ctl -p`(管线拓扑)/ `v4l2-ctl`(格式与抓流)/ `dmesg`(probe 与运行日志)
