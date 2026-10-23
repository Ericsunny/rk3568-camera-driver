# L1-B 对照报告:自写 OV8858 驱动 vs 厂商驱动

- 平台:YCKJ 硬创 iTOP-3568(RK3568),内核 4.19.232
- 传感器:OV8858,R2A 版本,2 lane,`ovti,ov8858-sm`(我们的驱动),`ovti,ov8858`(厂商驱动)
- 日期:2026-10-21
- 结论一句话:**两种模式都能出图,帧结构均 100% 有效(1632×1224 @29.3 fps、1920×1080 @29.3 fps)**

---

## 0. 摘要

| 项目 | 厂商驱动 | 我们(ov8858_sm) |
|---|---|---|
| 代码量 | 3676 行 | **666 行 .c + 449 行 .h** |
| 支持模式 | 4 张表(R1A/R2A × 2/4 lane)× 2 种分辨率 | 2 档(1632×1224、1920×1080) |
| 寄存器表(1632×1224,2 lane,R2A) | 311 + 30 条 | **311 + 30 条,逐条 0 差异** |
| 1080p | **没有这一档** | 自推(以厂商全分辨率表为骨架,13 改值 + 13 新增) |
| 出图 | 3264×2448 正常 | **两种模式都正常** |
| 实测帧率 | — | 1632×1224:**29.3 fps**;1920×1080:**29.3 fps** |
| 帧结构(每行是否有数据) | 2448/2448 行有效 | 1632×1224:**1224/1224**;1920×1080:**1080/1080** |

**最有价值的四件事**(不是"抄了一张表"):

1. 从 `-515` 这个错误码一路追到内核 `rkcif_update_sensor_info()` 的 `ret` 未清零,补出 `.video.g_mbus_config`
2. 抓出"偶发整帧垃圾"的真因——probe 上电后没断电,clk/regulator 引用计数不配对
3. 1080p 是自己从 datasheet 推的,并且**靠"每行非零字节数"这个判据发现帧结构是坏的**(肉眼完全看不出)
4. 缺行的真因是 **BLC anchor 寄存器越界**——厂商两档内部都读满 3264 列所以从不越界,我们是真裁剪,anchor 必须按窗口重算

---

## 1. 交付物

| 文件 | 行数 | 作用 |
|---|---|---|
| `code/ov8858_sm.c` | 666 | 驱动主体:subdev 五件套、上电时序、寄存器读写、模式切换、ctrl、电源管理 |
| `code/ov8858_sm_regs.h` | 449 | 全局表 311 条 + 1632×1224 表 30 条 + 1080p 表 39 条 |
| `code/Makefile` | 33 | 自动探测工具链的外编 Makefile |
| 内核改动 | — | `drivers/media/i2c/Kconfig` + `Makefile` + `.config` 里 `CONFIG_VIDEO_OV8858_SM=y` |

> 为什么是 `=y` 而不是模块:见手册 §5.4(4.19 的 async 是自底向上链式,sensor 晚到就永远配不上已经 probe 完的桥)。

---

## 2. 寄存器对照(逐条,非抽样)

比对方法:把两边的 `{addr, val}` 用正则解析成字典,再逐个 key 比较值和条数(脚本见 §8)。

### 2.1 全局表(global,2 lane / R2A)

| | 条数 | 值不一致 | 一方独有 |
|---|---|---|---|
| 厂商 `ov8858_global_regs_r2a_2lane[]` | 311 | — | — |
| 我们 `ov8858_sm_global_regs[]` | 311 | **0** | **0** |

**结论:完全一致。**

### 2.2 1632×1224 模式表

| | 条数 | 值不一致 | 一方独有 |
|---|---|---|---|
| 厂商 `ov8858_1632x1224_regs_r2a_2lane[]` | 30 | — | — |
| 我们 `ov8858_sm_mode_1632x1224_regs[]` | 30 | **0** | **0** |

**结论:完全一致。** 这一档的画质相关寄存器(模拟/DPC/BLC)是借用厂商已知-good 值的,符合事先约定的方案(自推时序 + 借用模拟块)。

### 2.3 1920×1080(厂商侧不存在)

厂商只有 3264×2448 和 1632×1224 两档,1080p 是我们自己做的。做法:**以厂商全分辨率表(29 条)为骨架,只做最小改动**,目的是让所有"看不懂的寄存器"(0x3830、0x3836、0x4600、0x4601、0x4500、0x382D 等)保持已知-good。

**改值 13 条**:

| 寄存器 | 厂商(3264×2448) | 我们(1920×1080) | 说明 |
|---|---|---|---|
| `0x0100` | `0x00` | `0x01` | 补上开流位(厂商是在函数里单独写的,不在表里) |
| `0x3501` | `0x9A` | `0x4C` | 曝光 2466 行 → 1226 行 |
| `0x3502` | `0x20` | `0xA0` | 同上 |
| `0x3808` | `0x0C` | `0x07` | 输出宽 3264 → 1920 |
| `0x3809` | `0xC0` | `0x80` | 同上 |
| `0x380A` | `0x09` | `0x04` | 输出高 2448 → 1080 |
| `0x380B` | `0x90` | `0x38` | 同上 |
| `0x380E` | `0x09` | `0x04` | VTS 2474 → 1236(30 fps) |
| `0x380F` | `0xAA` | `0xD4` | 同上 |
| `0x4022` | `0x0C` | `0x07` | **BLC anchor LEFT END**:3168 → 1863 |
| `0x4023` | `0x60` | `0x47` | 同上 |
| `0x4025` | `0x36` | `0x3E` | **BLC anchor RIGHT START**:1846 → 1086 |
| `0x4027` | `0x37` | `0xD5` | **BLC anchor RIGHT END**:2103 → 1237 |

**新增 13 条**:

| 寄存器 | 值 | 说明 |
|---|---|---|
| `0x3841` | `0x00` | 关掉 auto-size(全局表给的是 `0xFF`) |
| `0x3800/01` | `0x02B0` | H crop start = 688 |
| `0x3802/03` | `0x02BC` | V crop start = 700 |
| `0x3804/05` | `0x0A2F` | H crop end = 2607 |
| `0x3806/07` | `0x06F3` | V crop end = 1779 |
| `0x4020/21` | `0x0153` | **BLC anchor LEFT START** = 339(按窗口缩放) |
| `0x4024` | `0x04` | anchor RIGHT START 高位(全局表是 `0x0F`) |
| `0x4026` | `0x04` | anchor RIGHT END 高位(全局表是 `0x0F`) |

**没写的**:0 条(厂商全分辨率表里的寄存器我们一条没漏)。

---

## 3. 哪些是"我们自己推的"

### 3.1 像素率模型(从两种已知模式反推)

| 模式 | HTS 寄存器 | 有效 HTS | VTS | fps | 有效像素率 |
|---|---|---|---|---|---|
| 3264×2448(厂商) | 1940 | ×2 = 3880 | 2474 | 15 | 143.9 M |
| 1632×1224(厂商) | 1928 | ×2 = 3856 | 1244 | 30 | 143.9 M |

→ 芯片像素时钟固定 ≈ 143.9 Mpixel/s,**HTS 有效值 = 2 × 寄存器值**。这是后面所有时序计算的基准。

### 3.2 1080p 的时序与窗口

```
HTS = 1940(与全分辨率相同,见 3.4 的坑)
VTS = 1236  → 143.9e6 / (3880 × 1236) = 30.0 fps
曝光 = 1226 行 = VTS − 10
窗口 = 阵列 3272×2456 居中取 1920×1080 → H 688..2607,V 700..1779
```

板端回读验证:

```
start_stream[1080p]: stream=0x01 0x3814=0x01 0x3821=0x46 size=1920x1080 hts=1940 vts=1236
```

### 3.3 `.g_mbus_config`:从 errno 515 追到内核源码

现象:`open("/dev/video0")` 返回 `Unknown error 515`,dmesg 打 `update sensor info failed -515`。

根因在 `drivers/media/platform/rockchip/cif/capture.c:2769`:

```c
ret = v4l2_subdev_call(sensor->sd, video, g_mbus_config, &sensor->mbus);
if (ret && ret != -ENOIOCTLCMD) { ...; return ret; }   /* -515 被容忍 */
...
return ret;                                            /* 但 ret 没清零 */
```

我们驱动没实现 `.video.g_mbus_config` → 返回 `-ENOIOCTLCMD(-515)` → 内核"容忍"了却没清 `ret`,最后把它当自己的返回值抛出去。补上这个 op 即可。

**注意它在 `v4l2_subdev_video_ops` 里,不在 `pad_ops`**——按"五件套"想象时极易漏。

### 3.4 踩坑后的四个修正(每个都有实验支撑)

| 坑 | 现象 | 根因 | 修正 |
|---|---|---|---|
| HTS 按裁剪宽度缩小 | FS/FE mismatch、空行 | datasheet 4.2:"windowing 只是遮掉窗口外像素,**原有时序不变**" | HTS 保持 1940 |
| 手写窗口 + auto-size 同时存在 | buffer 隔行全 0 | 全局表 `0x3841=0xFF` 已开 auto-size,两者打架 | 二选一,不能同时用 |
| 曝光照抄全分辨率 | 整帧全白(均值 1022/1023) | 2466 行的曝光在 VTS=1236 下被钳到整帧 | 曝光 = VTS − 10 |
| `0x0100=0x01` 写在 `REG_NULL` 之后 | 回读 `stream=0x00`、抓 0 字节 | 写表循环遇到 `REG_NULL` 就退出 | stream-on 必须在 `REG_NULL` 之前 |

### 3.5 电源引用计数(偶发整帧垃圾)

原实现:probe 为了读 ID 上电,成功路径**不断电**。于是第一次开流再上电 → clk/regulator 计数变 2,关流只关一次 → 传感器一直带电,两次开流之间没有真正复位 → **偶发整帧垃圾**(实测同一配置有时均值 546、有时 288)。

修正:加 `bool powered` 把上电/断电做成幂等 + probe 读完成功后主动 `power_off`。

验证:空闲时 `clk_cam1_out enable=0 prepare=0`;连开 5 次流全部 `mean 545.0~545.5`、行间相关 `0.899~0.900`。

### 3.6 调试方法论(这部分也是产出)

1. **A/B 对照**:把 dts 的 `compatible` 换成 `ovti,ov8858`、打开 `CONFIG_VIDEO_OV8858=y`,让厂商驱动跑同一模式同一格式,用同一套脚本抓图对比 → 一次性判定"是我们的锅还是平台的锅"。
2. **帧结构判据**:不要只看"像不像图",要按行统计**非零字节数**——空行、半行、缺行一眼就看出来(见 §5.3)。

---

### 3.7 缺行真因:BLC anchor 在裁剪窗口里越界(当天定位并修好)

**现象**:1080p 帧里 75.6%~83.4% 的行有数据,其余整行全零,且空行集中在前 353 行、每隔一行出现一次。帧率、CIF 统计、图像肉眼都没问题——**只有按行统计非零字节数才看得出来**。

**推理**:`0x4020/21`、`0x4022/23`、`0x4024/25`、`0x4026/27` 是 datasheet §6.12 的 **ANCHOR LEFT/RIGHT START/END**,单位是**输出像素坐标**。厂商两档模式之所以能一直用大数值:

| 模式 | 内部实际读出宽 | 厂商给的 ANCHOR LEFT END | 是否越界 |
|---|---|---|---|
| 3264×2448 | 3264 | `0x0C60` = 3168 | 否 ✓ |
| 1632×1224 | **内部仍读 3264**(2×2 binning) | `0x0600` = 1536 | 否 ✓ |
| 1920×1080(我们真裁剪) | **只有 1920 列** | 照抄来的 3168 | **是 ✗** |

**修法**:按 `1920/3264 = 0.588` 把四个 anchor 缩放回窗口内。

```c
{0x4020, 0x01}, {0x4021, 0x53},   /* LEFT  START = 339  (576  × 0.588) */
{0x4022, 0x07}, {0x4023, 0x47},   /* LEFT  END   = 1863 (3168 × 0.588) */
{0x4024, 0x04}, {0x4025, 0x3e},   /* RIGHT START = 1086 (1846 × 0.588) */
{0x4026, 0x04}, {0x4027, 0xd5},   /* RIGHT END   = 1237 (2103 × 0.588) */
```

**验证**:1080p 连跑 30 帧,每帧 1080 行 **零空行**;1632×1224 回归 10 帧同样零空行;帧率 29.3 fps。

**这条经验值得单独记**:裁剪模式不是"改小 0x3808/0x380A 就行",任何**以输出像素为单位的旁路寄存器**(BLC anchor、zline/blkline 位置等)都必须跟着窗口一起换算。datasheet 里这些寄存器只给了名字和默认值、没有换算公式,只能靠"厂商两档模式的内部读出宽"这个隐含前提去推。

---

## 4. 驱动架构对照

| 能力 | 厂商 ov8858.c | 我们 ov8858_sm.c | 差距影响 |
|---|---|---|---|
| 代码量 | 3676 行 | 1115 行 | — |
| `core.s_power` | 有 | 无 | 我们靠 s_stream 里开关电,够用 |
| `core.ioctl` | GET_MODULE_INFO / AWB_CFG / LSC_CFG / SET_QUICK_STREAM | GET_MODULE_INFO / SET_QUICK_STREAM | 少了 3A 配置入口(要配 rkaiq 才需要) |
| `core.compat_ioctl32` | 有 | **有(补的,见 §10)** | — |
| `video.s_stream` | 有 | 有 | — |
| `video.g_mbus_config` | 有 | **有(补的,见 §3.3)** | — |
| `video.g_frame_interval` | 有 | 有（补的，§11） | — |
| `pad.enum_mbus_code` | 有 | 无 | 应用层枚举不到格式 |
| `pad.enum_frame_size` | 有 | **有(补的,见 §10)** | — |
| `pad.enum_frame_interval` | 有 | 有（补的，§11） | — |
| `pad.get_fmt / set_fmt` | 有 | 有 | — |
| ctrl: LINK_FREQ / PIXEL_RATE | 有 | 有 | — |
| ctrl: EXPOSURE / ANALOGUE_GAIN | 有 | 有 | — |
| ctrl: HBLANK / VBLANK | 有 | 无 | 应用层调不了消隐 |
| ctrl: TEST_PATTERN | 有 | 无 | 少了自测手段 |
| OTP 读取与应用 | 有(198 处引用) | 无 | 画质(模块级校准)缺失 |
| pm_runtime | 有 | **有(补的,L4 阶段)** | — |
| 模式表 | R1A/R2A × 2/4 lane × 2 分辨率 | 2 档(不够时加表即可) | 只支持自己这颗 R2A 2-lane |

---

## 5. 实测结果

### 5.1 启动链路

```
[ 1.541976] ov8858-sm 5-0036: resources: xvclk=24000000 Hz, pwdn=ok, reset=ok
[ 1.547037] ov8858-sm 5-0036: Detected OV8858 id 0x8858
[ 1.550733] rockchip-csi2-dphy csi2-dphy2: dphy2 matches ov8858-sm 5-0036:bus type 4
```

media 链路:`ov8858-sm(/dev/v4l-subdev2) → rockchip-csi2-dphy2 → rockchip-mipi-csi2 → stream_cif_mipi_id0(/dev/video0)`,全部 ENABLED。

### 5.2 两种模式

| 模式 | 回读 | 实测帧率 | 空行 | 状态 |
|---|---|---|---|---|
| 1632×1224 | `hts=1928 vts=1244` | **29.3 fps** | **0 / 1224** | ✅ 可交付 |
| 1920×1080 | `hts=1940 vts=1236` | **29.3 fps** | **0 / 1080** | ✅ 可交付 |

### 5.3 怎么判断"帧结构好不好"(关键判据)

抓一帧原始数据,按行统计"非零字节数"即可:

| 配置 | 全零行 | 有效行占比 |
|---|---|---|
| 厂商驱动 3264×2448 | 0 / 2448 | 100% |
| 我们 1632×1224 | 0 / 1224 | 100% |
| 我们 1920×1080(**修 anchor 前**) | 179~263 / 1080 | 75.6%~83.4% |
| 我们 1920×1080(**修 anchor 后**) | **0 / 1080** | **100%** |

修 anchor 前的空行集中在前 353 行、**每隔一行空一行**,肉眼几乎看不出来(那一片只是显得更暗更脏),但结构是坏的;修完后 30 帧连跑每帧 1080 行零空行。

![1080p 行有效性:修复前(179/1080 空行)与修复后(0/1080)](evidence/2026-10-21-anchor修复前后对比.png)

### 5.4 已排除的可能(全部单变量实验)

| 假设 | 实验 | 结果 |
|---|---|---|
| 垂直消隐太大 | 1632×1224 把 VTS 1244 → 2000 | **0 次边界错误** → 与消隐无关 |
| 内存 stride 有 160 字节对齐填充 | 1080p 换 `BYR2`(stride 恰好 3840 = 1920×2) | 空行数**完全相同 179** → 与 stride 无关 |
| 手写窗口与 auto-size 冲突 | 两种配置各测一遍 | 都坏(263 / 179),只是坏的程度不同 |
| HTS 太小 | 改成全分辨率同值 1940 | 边界错误依旧 |
| 曝光越界 | 改成 VTS − 10 | 只影响亮度,与缺行无关 |
| **BLC anchor 越界** | **按 1920 窗口重算 `0x4020~0x4027`** | **✅ 命中:空行 179 → 0** |

---

## 6. 诚实清单:还差什么

**功能层面**
1. ❌ OTP / LSC → 模块级画质校准仍缺(要读 OTP 并套 LSC,属于"产品级画质"范畴)
2. ✅ `pm_runtime` → **L4 阶段已补**(开机 probe 完 → 3.5 s 后 suspended;开流 → active;关流 → 2 s 后 suspended)
3. ✅ `enum_frame_size` / `compat_ioctl32` → **2026-10-21 补完并实测**(见 §10);
   仍缺 `g_frame_interval` / `pad.enum_frame_interval`(应用层查不到帧率,不影响出图)
4. ❌ test pattern / hblank / vblank ctrl → 调试手段少

**1080p 模式**
- ✅ 帧结构已修好(30 帧连跑零空行),帧率 29.3 fps
- ⚠️ 仍存在:每帧一条 `ERR1: error matching frame start with frame end, reg: 0x10`。来自 **Synopsys DW CSI-2 主控**的 `ERR1(0x20)` 寄存器 `err_bndry_match` 字段(bits 7:4);`mipi-csi2.c` 只在 DSI-RX 路径才屏蔽这一位(`&= 0xe00000f0`),CSI-2 路径 `MSK1=0` 全开。CIF 侧 `all err count: 0`、`frame dma end: 30/30`,数据通路无影响,属主控边界检查的误报——1632×1224 模式下不会出现

**其他**
- 只验证了 R2A + 2 lane 这一种组合
- 本驱动在 BSP 里是 **`CONFIG_VIDEO_OV8858_SM=y`(内建)**,改代码必须重编 `Image` 刷 `/boot`,不能 `insmod`(踩坑记录见 §10.1)

---

## 7. 面试可以怎么讲(四个故事)

1. **`-515` 的故事**:用户态只看到一个 errno,我一路追到内核 `rkcif_update_sensor_info()`,发现它"容忍了 `-ENOIOCTLCMD` 却忘了清 `ret`",于是补出 `.video.g_mbus_config`。→ 体现:能从错误码穿透到内核源码,而不是靠猜。
2. **偶发垃圾帧**:同一份代码有时出图有时出噪声。抓出真因是 probe 上电后没断电导致 clk/regulator 引用计数不配对,把上电/断电做成幂等,并用 `clk_summary` 的计数和 5 次连流的统计证明修好。→ 体现:会做**可证伪**的定位,而不是"重启试试"。
3. **1080p 自推 + 单变量排除**:从 datasheet 4.2 那句"windowing 不改时序"推出 HTS 不能随宽度缩小;再用"VTS 拉大 / 换 16bit 容器 / 开关 auto-size"三个单变量实验,把消隐、stride、窗口冲突逐个排除。→ 体现:会用对照实验缩小问题空间。
4. **"图像看着正常但结构是坏的"**:1080p 的图肉眼完全没问题,是我按行统计非零字节数才发现 24% 的行是空的。真因是 **BLC anchor 寄存器在裁剪窗口里越界**——厂商两档内部都读满 3264 列所以从不越界,只有真裁剪才会踩到。→ 体现:会为"看不见的缺陷"找可量化的判据,并且能从一个"厂商从不触发"的隐含前提里推出正确取值。

---

## 8. 复现与校验命令

**寄存器表逐条比对**(在 VM 上跑):

```bash
python3 - <<'PY'
import re
V='/home/rk3568/rk3568/RK_SDK/kernel/drivers/media/i2c/ov8858.c'
O='/home/rk3568/work/ov8858_sm/ov8858_sm_regs.h'
def block(t,n):
    i=t.index(n); j=t.index('};',i); return t[i:j]
def parse(t):
    return [(int(a,16),int(b,16)) for a,b in
            re.findall(r'\{\s*0x([0-9a-fA-F]+)\s*,\s*0x([0-9a-fA-F]+)\s*\}', t)]
vg=parse(block(open(V).read(),'ov8858_global_regs_r2a_2lane[]'))
vm=parse(block(open(V).read(),'ov8858_1632x1224_regs_r2a_2lane[]'))
og=parse(block(open(O).read(),'ov8858_sm_global_regs[]'))
om=parse(block(open(O).read(),'ov8858_sm_mode_1632x1224_regs[]'))
for name,a,b in (("GLOBAL",vg,og),("MODE",vm,om)):
    da,db=dict(a),dict(b)
    print(name,"条数",len(a),len(b),
          "值不同",sum(1 for k in da if k in db and da[k]!=db[k]),
          "独有",len([k for k in da if k not in db]),len([k for k in db if k not in da]))
PY
```

**抓帧**(板端):

```bash
# 1632x1224
media-ctl -d /dev/media0 --set-v4l2 '"ov8858-sm 5-0036":0[fmt:SBGGR10_1X10/1632x1224]'
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1632,height=1224,pixelformat=BG10
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=/tmp/a.raw

# 1080p(必须先设 sensor 子设备)
media-ctl -d /dev/media0 --set-v4l2 '"ov8858-sm 5-0036":0[fmt:SBGGR10_1X10/1920x1080]'
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=BG10
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=/tmp/b.raw
```

**解包 + 帧结构检查**(PC 端,python3 + numpy):

```python
import numpy as np
BPL,H,W=2048,1224,1632          # 1080p 用 2560/1080/1920
b=np.fromfile('a.raw',dtype=np.uint8).reshape(-1,BPL)[:H,:W*10//8]
print("空行:",(b!=0).sum(axis=1).__eq__(0).sum(),"/",H)
g=b.reshape(H,-1,5).astype(np.uint16)     # compact RAW10: 5 字节 -> 4 像素
p=np.stack([(g[:,:,0]<<2)|(g[:,:,4]&3),(g[:,:,1]<<2)|((g[:,:,4]>>2)&3),
            (g[:,:,2]<<2)|((g[:,:,4]>>4)&3),(g[:,:,3]<<2)|((g[:,:,4]>>6)&3)],2).reshape(H,-1)[:,:W]
print("像素范围", p.min(), p.max())       # 正常: 0..1023
```

---

## 9. 证据文件

| 文件 | 说明 |
|---|---|
| `evidence/2026-10-21-第一帧-我们的驱动-1632x1224.png` | 我们的驱动抓到的第一帧 |
| `evidence/2026-10-21-对照-厂商驱动-1632x1224.png` | 厂商驱动同模式对照图 |
| `evidence/2026-10-21-1080p30-修复后.png` | 1080p 修复 anchor 后的出图 |
| `evidence/2026-10-21-anchor修复前后对比.png` | **关键证据**:每列=一行,修复前 179 行空、修复后 0 行空 |
| `evidence/2026-10-21-1080p-行有效性-黑=空行.png` | 修复前的行有效性条带(单张) |
| `evidence/2026-10-21-1080p30-天花板.png` | 修复前的 1080p 出图(肉眼也正常,但结构是坏的) |
| `code/ov8858_sm.c` / `code/ov8858_sm_regs.h` / `code/Makefile` | 源码快照 |
| `16-模块四动手手册-一步一步版.md` | 全过程手册(§5.9~§5.14 是本次 L1-B 的记录) |

---

## 10. 收尾:补 `enum_frame_size` / `compat_ioctl32`(2026-10-21)

### 10.1 先踩一个坑:驱动是**内建**的,不是模块

`zcat /proc/config.gz | grep OV8858` → **`CONFIG_VIDEO_OV8858_SM=y`**。

所以把 `.ko` 装进 `/lib/modules/4.19.232/extra/` + `depmod` **完全无效**(`modprobe` 只回 `module is already loaded`);
而且这个内核的 `lsmod` 什么都不打印、`/proc/modules` 是空的,`/sys/module/ov8858_sm/` 只有 `parameters` 和 `uevent`(没有 `sections`/`refcnt`)——这些都是内建特征。
**这就是之前一整天"改了代码但枚举不出分辨率"的真因:板上跑的一直是旧的编译进 Image 的代码。**

正确流程:

```bash
# VM 上
cp /home/rk3568/work/ov8858_sm/ov8858_sm.c \
   /home/rk3568/rk3568/RK_SDK/kernel/drivers/media/i2c/ov8858_sm.c
cd /home/rk3568/rk3568/RK_SDK/kernel
TC=$(ls -d ../prebuilts/gcc/linux-x86/aarch64/gcc-buildroot-9.3.0-2020.03-*/ | head -1)
export LD_LIBRARY_PATH=${TC}lib
make ARCH=arm64 CROSS_COMPILE=${TC}bin/aarch64-rockchip-linux-gnu- -j6 Image
# 刷机:scp arch/arm64/boot/Image → 板 /boot/Image-4.19.232(nano boot.cmd: load /Image-4.19.232)
```

**`/boot` 只有 124 MB 且原本 100% 满**:先删半截的 `Image-4.19.232.bak2`,再把旧 Image 改名 `.bak1`(留一份可回滚),
`Image-4.19.232.bak-factory`(26071048 B)是原厂镜像,**不要动**。当前 Image:`md5 5f10a688f232bd34b622fa4df98dd263`,
内核版本 `#32 SMP Wed Oct 21 14:38:47 UTC 2026`。

### 10.2 `enum_frame_size`

```c
static const struct ov8858_sm_frmsize {
	u32 width;
	u32 height;
} ov8858_sm_frmsizes[] = { {1632, 1224}, {1920, 1080} };

static int ov8858_sm_enum_frame_size(struct v4l2_subdev *sd,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad != 0)
		return -EINVAL;
	if (fse->index >= ARRAY_SIZE(ov8858_sm_frmsizes))
		return -EINVAL;
	fse->min_width  = fse->max_width  = ov8858_sm_frmsizes[fse->index].width;
	fse->min_height = fse->max_height = ov8858_sm_frmsizes[fse->index].height;
	return 0;
}
```

验收(用户在 `/dev/v4l-subdev2` 上直接发 ioctl):

| 请求 | 结果 |
|---|---|
| `index=0` | min/max 1632×1224 |
| `index=1` | min/max 1920×1080 |
| `index=2,3` | `EINVAL` |
| `which=5`(非 TRY/ACTIVE) | `EINVAL` |
| `pad=3` | `EINVAL` |

**两个容易误判的坑**:
1. 本内核(4.19.232 + Rockchip 回移)的 `struct v4l2_subdev_frame_size_enum` **带 `which` 字段**,`sizeof = 64`,
   `VIDIOC_SUBDEV_ENUM_FRAME_SIZE = 0xC040564A`。按老结构算 60 字节发过去,v4l2 核心按 `_IOC_SIZE` 校验直接回 `-ENOTTY`,
   看起来就像"驱动没实现"。
2. `v4l2-subdev.c` 派发前还会检查 `which ∈ {V4L2_SUBDEV_FORMAT_TRY(0), V4L2_SUBDEV_FORMAT_ACTIVE(1)}` 且 `pad < num_pads`。

> 注:`/dev/video0 --list-formats-ext` 仍显示 `Stepwise 64x64 - 1632x1224 with step 8/8`,那是 **rkcif 自己的**上限,不走 sensor 的枚举,所以看不到 1920×1080。

### 10.3 `compat_ioctl32`:64 位没事,32 位直接把内核打崩

```c
#ifdef CONFIG_COMPAT
static long ov8858_sm_compat_ioctl32(struct v4l2_subdev *sd,
				     unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	u32 stream;
	long ret;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf)
			return -ENOMEM;
		ret = ov8858_sm_ioctl(sd, cmd, inf);
		if (!ret && copy_to_user(up, inf, sizeof(*inf)))
			ret = -EFAULT;
		kfree(inf);
		return ret;
	case RKMODULE_SET_QUICK_STREAM:
		if (copy_from_user(&stream, up, sizeof(stream)))
			return -EFAULT;
		return ov8858_sm_ioctl(sd, cmd, &stream);
	default:
		return -ENOIOCTLCMD;
	}
}
#endif
```

**为什么 64 位从来没炸**:`v4l2-subdev.c` 的 `subdev_ioctl()` 走 `video_usercopy()`,它把用户参数整块拷进内核缓冲再调驱动,
所以驱动里对"指针"做 `memset` 恰好写在内核副本上;而 `subdev_compat_ioctl32()` 只是把 `arg` **原样透传**,
`ov8858_sm_get_module_inf()` 里的 `memset(inf, 0, sizeof(*inf))` 就变成了"对用户指针普通写"→ PAN:

```
Unable to handle kernel access to user memory outside uaccess routines at virtual address 00000000ff8840a0
Call trace:
 __memset
 ov8858_sm_compat_ioctl32+0x54/0x6c
 subdev_compat_ioctl32+0x54/0x78
 v4l2_compat_ioctl32+0x2310/0x2438
 __arm64_compat_sys_ioctl+0x190/0xdb8
```

修复后(内核 `#32`)实测:

```
# 64 位静态
ptr_bits=64 sizeof(rkmodule_inf)=5203 dev=/dev/v4l-subdev2
RESULT=OK sensor='ov8858-sm' module='ov8858-sm' lens='default'
# 32 位静态(armhf,静态链接,-static)
RESULT=OK sensor='ov8858-sm' module='ov8858-sm' lens='default'
# dmesg 里 oops 计数 0
```

### 10.4 这一项的结论

- 两处都**真补上了**,而且在**真实内核**里验过(不是 `insmod` 一个新模块骗自己)。
- `compat_ioctl32` 这项的价值不在"多一个回调",而在于它**暴露了驱动原有的一个真 bug**(对用户指针直接 `memset`)。
  如果客户用 32 位 rootfs 调摄像头应用,原驱动会**直接 oops**,这个坑比缺拟合字号严重得多。

## 11. 帧间隔三件套（补 `video.g/s_frame_interval` + `pad.enum_frame_interval`）

### 为什么补
2022 版 rkaiq 在 `get_sensor_caps` 阶段会枚举传感器帧间隔，我们没实现，它一直打印：

```
[CAMHW] CamHwIsp20.cpp:176: @get_sensor_caps /dev/v4l-subdev2: Enum sensor frame interval failed
```

### 本内核的 ABI 陷阱（关键）
4.19 + Rockchip 回移与新版 V4L2 不同，照新手册写会编不过：

| 位置 | 本内核 | 新版 |
|---|---|---|
| G/S_FRAME_INTERVAL | **video ops**（`sd, interval`） | pad ops（多一个 pad_config） |
| ioctl 号 | **`_IOWR('V', 21/22)`** | `_IOWR('V', 85/86)` |
| 结构体字段 | `struct v4l2_fract interval` | 有 min/max 语义 |
| enum 结构体 | 只有单个 `interval` | 同 |

且内核按 `_IOC_SIZE` 严格匹配命令号——尺寸不对直接 `ENOTTY`，极易误判成"没实现"。
实测（`tools/fi_probe.c`）：

```
size=56 -> -ENOTTY
size=64 -> 0          ← 只认 64
size=72..96 -> -ENOTTY
```

### 实现
`ov8858_sm.c` 新增 `ov8858_sm_timings[]`（1632×1224: HTS 1928/VTS 1244；1920×1080: HTS 1940/VTS 1236），

```
fps = pixel_rate / (2 * HTS * VTS)     → 30.02 / 30.03 fps
```

帧率不是可调量，所以 `s_frame_interval` 只接受当前值（±20% 内），否则写回实际值并返回 `-EINVAL`。

### 验证（kernel #43，`tools/fi_test.c`）
```
enum[0] = 1/30      enum[1] = 1/30      enum[2] = EINVAL
get = 1/30          set 1/30 -> 0       set 1/60 -> EINVAL
RESULT=PASS fails=0
```

### rkaiq 侧结果
用 `LD_PRELOAD=tools/ioctlspy.c` 抓到库的真实调用（`nr=75 size=64`）：

```
idx=0 ret=0   idx=1 ret=0   idx=2 ret=-1(EINVAL)   ← 正常终止枚举
nr=21 (G_FRAME_INTERVAL) size=48 ret=0
```

修好后新日志里 `Enum sensor frame interval failed` = **0 条**。
（注意：`journalctl` 里仍能看到 1 条旧记录，其时间戳 04:56:33 比当前板上时间 04:36:33 还晚——那是上次启动留下的，不是新产生的。）
