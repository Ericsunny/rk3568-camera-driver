# I²C 排障：物理接口与总线映射

> 2026-10-07 调试记录：从 sensor ID 读取异常到 media 管线挂载。
> 排查期间的关键问题是摄像头实际连接 J11（CAM1/I2C5），而初始检查针对 J10（CAM0/I2C1）。后续切换接口并完成链路检查；设备节点对应关系以当时环境为准。

## 1. 最终状态(2026-10-07,L1-A Phase 2 收官)

```
dmesg(开机):
  imx415 1-001a: Detected imx415 id 0000e0                    ← probe 通过
  rockchip-csi2-dphy csi2-dphy1: dphy1 matches m00_b_imx415 1-001a:bus type 4
  imx415 1-001a: set fmt: cur_mode: 3864x2192, hdr: 0, bpp: 10  ← 已在写传感器寄存器
  imx415 1-001a: set exposure(shr0) 2149 = cur_vts(2250) - val(101)

media-ctl(-d /dev/media0):
  m00_b_imx415 1-001a (v4l-subdev3, subtype Sensor)
    pad0 [SGBRG10_1X10/3864x2192@10000/150000] → rockchip-csi2-dphy1:0 [ENABLED]
    → rkisp-csi-subdev → rkisp-isp-subdev → mainpath(/dev/video0) / selfpath(/dev/video1)
  /dev/v4l-subdev0~3 全在
```

- 默认工作模式:**3864x2192 @ 15fps,SGBRG10(10bpp),2-lane**(`detect imx415 lane 2`,来自 endpoint `data-lanes <1 2>`)
- 裁剪 (12,16) 起点 → 3840x2160 有效区(传感器有效阵列外围的 dummy 像素)

## 2. 误判修正链(每个假设→证据→证伪→修正)

| # | 当时假设 | 依据 | 证伪证据 | 修正 |
|---|---------|------|---------|------|
| 1 | J10 第 12 脚=GND,与转接板 J1-12(5V 输入)对插=5V 网络短路 | YC 原理图坐标重建(kb05 标了"待万用表确认") | **J10 高清重读**:GND 汇流排结点只在 1/4/7/10/16/17,12 脚无线无结点=**NC** | 撤销短路警报,"胶带绝缘 12 脚"方案作废 |
| 2 | 模组没电(转接板 5V 网络无源) | J1-12=5V 输入,板 12 脚 NC,无人喂电 | **两条总线 i2cdetect 都扫出 1a** | 模组可响应 I²C；转接板的具体供电路径仍有疑问，见 §5 |
| 3 | PWDN 悬空→传感器休眠 | imx415 驱动只认 power/reset-gpios 不认 pwdn-gpios(dts 恰配了 pwdn) | **模组原理图:IMX415 芯片没有硬件 PWDN 脚**;模组 PWDN 只连两颗不贴片的 NC 电阻(LDO EN);XCLR 二极管电路模组内自管 | PWDN 全程无关;驱动忽略 pwdn-gpios 无害,dts 不用改 |
| **真** | **物理口=J10** | 物理接口未先核实 | 现场确认实际连接 J11 | J11 对应 I2C5，`i2cdetect -y 5` 扫出 1a |

**误判 2 的教训展开**:"VDD_5V" 是模组输入脚的**标称名**,不是硬性 5V 约束——模组内 U2(MT3520B)是 LDO 不是 buck(输出端是 100R@100MHz 磁珠,不是功率电感),3.3V 输入时压差 ~0.1-0.3V 也能出 ~3.1V 供后级 XC6221×3(2.8/1.8/1.2V)正常工作。**看到"5V"别急着断定"板子 3.3V 不够",先看模组内电源拓扑是 buck 还是 LDO。**

## 3. 物理口与总线映射（历史设备树）

| 板座 | 相机 | I2C 控制器 | /dev 节点 | sensor 节点(dts) | PWDN GPIO | reset GPIO |
|------|------|-----------|-----------|------------------|-----------|------------|
| **J10** | CAM0 | i2c1(fe5a0000) | **/dev/i2c-1** | `imx415@1a`(okay)| GPIO0_C6(=22)| GPIO1_B0(=40)|
| **J11** | CAM1 | i2c5(fe5e0000) | **/dev/i2c-5** | cam1_* 节点(ov5647/ov8858/sc132gs 等) | GPIO1_B1(=41)| GPIO1_B2(=42)|

- 两座引脚序相同:2/3=D0N/P,5/6=D1N/P,8/9=CLKN/P,11=PWDN,13/14=I2C_SCL/SDA,15=VCC3V3,1/4/7/10=GND,**12=NC**,16/17=MTH1/2
- 通用.dts 的 `&i2c5 { status = "okay" }` 一直在——/dev/i2c-5 从第一天就存在,却被无视了一整场
- **RK GPIO 编号** = bank×32 + 组×8 + 序(A=0,B=1,C=2):GPIO0_C6=22,GPIO1_B0=40

## 4. bring-up 纪律(从这次实战提炼)

1. **先核对物理接口与总线映射**：结合设备节点、设备树、板座丝印和原理图，再决定从哪条总线开始检查供电、复位及时序。
2. **把所有总线都扫一遍再下结论**——两条 i2cdetect 命令就能避免整场误判。
3. **用可工作的参考配置做对照**：记录模组连接、设备树与供电条件，再逐项比较差异。
4. **Rockchip V4L2 管线只在开机 probe 时建链**(v4l2-async notifier 一次性 complete):手动 `bind` 晚到的 subdev **不会**补建 media 链路(dmesg 也没有新动作)。验证 probe 成功用 `reboot`,bind 只用来证明 I2C 通。
5. **sysfs gpio export 会占住引脚**:之后驱动 `devm_gpiod_get` 拿同一个脚会 EBUSY(dmesg 多一条 `Failed to get reset-gpios`)。调试用的 export 用完记得 unexport 或重启。
6. **驱动 GPIO 属性名约定各厂不同**:imx415.c 只认 `power-gpios`/`reset-gpios`(3335/3338 行),无视 pwdn-gpios。**配 dts 前先 grep 驱动源码的 devm_gpiod_get**,别按通用惯例猜。
7. **驱动日志判读**:`dummy regulator`/`no pinctrl`/`Failed to get power-gpios` 都是 warn 不是 error(dts 无 supply 时模块硬件直通属预期);真正的失败信号是 `Unexpected sensor id` / `probe failed` / `No link between dphy and sensor`。

## 5. 硬件事实库(本次读图所得,存疑处已标)

**YC 板(btb_rk3568_mb_1v0.pdf p12)**:J10/J11=CON_FPC_15_1R0;I2C 均有板上 4.7K 上拉到 VCC3V3(R140-143);CAM0_PWDN=GPIO0_C6、CAM1_PWDN=GPIO1_B1(蓝字标注在图上)。

**野火 IMX415 模组(EBF410358V1R1_SCH)**:
- 电源:VDD_5V(1/2 脚)→ MT3520B(LDO,FB=0.6V,R4 200K/R7 44.2K → 3.315V)→ VDD_3V3 → XC6221A282(2.8V)/XC6221A182(1.8V)/XC6221B122(1.2V);1.8/1.2V 的 EN 均经 1K 上拉,PWDN 只经 NC 电阻连 EN(=无功能)
- I2C:2SK3018 ×2 做 3.3V↔1.8V 双向电平转换,模组侧自带 4.7K 上拉;FPC 3/4=SDA/SCL
- XCLR:RSTn(FPC 23)→ RB521S30 二极管电平移动 → XCLR;RSTn 悬空时模组内上拉释放复位——**转接链无 RST 连线也无妨**
- 晶振 Y1 37.1254MHz(VCC=DOVDD_1V8)→ R15 22R → INCK;SLAMODE 接地固定
- 模组 FPC 引脚:1/2=VDD_5V,3/4=I2C,9/10=CLKP/N,12/13=D0P/N,15=PWDN,23=RSTn(与"24P 表 16=PWDN"的截图计数差 1,以原理图为准)

**转接板(野火树莓派摄像头转鲁班猫)**:J1=15P(宿主)/J2=24P(模组)/J3=2×15 BTB(鲁班猫式宿主口);板载 RT8097(5V→3.315V buck)。**存疑:模组电源具体从 J1-15(3V3)还是 J1-12(5V 网络)取**——实测模组在 YC 板(12 脚 NC,只有 3.3V)上有电,说明 3.3V 路径存在;待拿到转接板 SCH 补上图。

## 6. 后续记录与待核实项

后续出图过程见[第一帧采集](first-frame.md)，双摄拓扑见[双摄采集记录](../pipeline/dual-camera.md)。转接板 3.3 V 到模组的具体供电路径仍需对照转接板原理图确认。
