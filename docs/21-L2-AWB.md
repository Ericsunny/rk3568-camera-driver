# 21 - L2:ISP 真出图,补 AWB 偏色(2026-10-21)

> **目标**:让 IMX415 经 rkisp 出的 NV12 **不再是绿的**,用正规 3A/WB 通路。
> **结果**:通过 **ISP params 通路**下发白平衡增益,色偏从 `U−21.4 / V−27.0` 收敛到 `U−1.4 / V−0.9`。

![before/after](evidence/2026-10-21-L2-AWB对比.png)

---

## 1. 问题定义:偏色到底偏在哪

用 BT.601 的口径量一帧 NV12 的色度均值,理想白平衡是 **U≈V≈128**:

| 状态 | Y 均值 | U 均值 | V 均值 |
|---|---|---|---|
| 无任何 WB(rkisp 默认参数) | 116.9 | **106.6(−21.4)** | **101.0(−27.0)** |
| 修好后 | 142.3 | **126.6(−1.4)** | **127.1(−0.9)** |

U、V 同时低于 128 = 蓝、红分量都偏少 = **画面整体偏绿**(G = Y − 0.344U − 0.714V,所以 U/V 低 → G 高)。这是 sensor 原始响应的典型特征,必须靠 WB 增益补偿。

---

## 2. 四条路都试了(按代价从小到大)

### 2.1 rkaiq(IQ 文件通路)——能跑,但崩

板上环境齐全:`/usr/lib/librkaiq.so`、`/usr/bin/rkaiq_3A_server`、36 个 IQ 文件,其中
`imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json` 与我们的模组名**完全匹配**。

启动后日志:

```
DBG: get rkisp-isp-subdev devname: /dev/v4l-subdev3
DBG: get rkisp-input-params devname: /dev/video13
DBG: get rkisp-statistics devname: /dev/video12
DBG: get rkisp_mainpath devname: /dev/video5
rk_aiq_uapi_sysctl_init success. iq:/etc/iqfiles//imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json
rk_aiq_uapi_sysctl_prepare success. mode:0
rk_aiq_uapi_sysctl_start success.
rkisp_init engine succeed
```

**AWB 确实是有效的**:那一轮抓到的帧 `U−1.2 / V−1.6`,色彩中性 ✓

但两个致命问题:
1. `ANALYZER:E:AWB stats invalid, ignore` —— 统计拿不到有效数据
2. **`rkaiq_3A_server` 会段错误退出**(无 pstore 记录),AE 把曝光压到 101,Y 掉到 21.6 → 画面又暗又没收敛

> 注:media0(rkcif,OV8858)那条 `parse sensor entity name ov8858-sm 5-0036 error` 是正常的——rkaiq 只支持 rkisp,遇到 rkcif 会 skip。

### 2.2 mini3a(我们自己写的 3A)——收敛了但增益是 1.0

```
fmt: 1920x1080 fourcc=NV12 sizeimage=3110400
>>> 收官帧已保存 final.nv12 (3110400 字节, 最终增益 g_r=0x100 g_b=0x100)
AE+AWB 双收敛,共 150 帧。
```

跑完 150 帧、AE/AWB 都报收敛,但**最终增益 g_r=g_b=1.0**,实测输出仍是 `U−21.3 / V−26.7` —— 等于没调。这是 mini3a 的 AWB 判据问题(它认为无需调整),不是通路问题。

### 2.3 isp_wb(内核模块直写寄存器)——无效,且 rmmod 会挂

`mini3a/isp_wb.c` 是直接写 ISP 寄存器(`0x0138/0x013c/…`)的模块。实测:

```
echo 0x180 > /sys/module/isp_wb/parameters/gain_r    # 回读 384 ✓
echo 0x140 > /sys/module/isp_wb/parameters/gain_b
→ 帧统计 U/V 一点没变(−21.6/−27.0,与基线完全一致)
```

原因:**ISP 每帧从 params 缓冲重新锁存**,寄存器直写会被覆盖。
另外 `rmmod isp_wb` **直接挂死**(dmesg 里 `Task dump ... rmmod R running`)——正好印证模块注释里的警告:"ISP 断流后驱动 runtime suspend 会关时钟,此时 readl/writel = 总线挂死"。

### 2.4 params 通路(正解)——生效

`mini3a/params_poke.c` 走的是**官方通路**:填 `struct isp21_isp_params_cfg` → `QBUF` 到 params 节点 → 内核 `awbgain_config()` 在帧边界写寄存器。

**踩到的第一个坑**:工具里 `PARAMS_DEV` 硬编码 `/dev/video8`,但这套系统的 params 节点已经变成 **`/dev/video13`**(rkaiq 日志里那句 `get rkisp-input-params devname: /dev/video13` 正好印证)。直接跑会 `G_FMT: Invalid argument`。

改掉之后按测出的偏色算增益:由 `V = 128 + 0.713(R−Y)`、`U = 128 + 0.564(B−Y)` 反推需要的 R/B 增益约 1.48/1.50,先打 `0x17B/0x180`:

| 增益 | U 偏差 | V 偏差 |
|---|---|---|
| 无 | −21.3 | −26.8 |
| R=0x17B(1.48) B=0x180(1.50) | −10.2 | −12.9 |

只补了一半 → 线性外推再补:`R ≈ 1.92(0x1EC)`、`B ≈ 1.94(0x1F0)`:

| 增益 | Y | U | V |
|---|---|---|---|
| **R=0x1EC B=0x1F0** | 142.3 | **126.6(−1.4)** | **127.1(−0.9)** |

**收敛。** 这就是上面那张对比图的来源。

---

## 3. 结论与经验

| 通路 | 是否生效 | 问题 |
|---|---|---|
| rkaiq + IQ | 部分(AWB 有效) | 统计无效 + **段错误** + AE 失控 |
| mini3a | ❌ | AWB 判据退化成增益 1.0 |
| isp_wb 直写寄存器 | ❌ | 每帧被 params 覆盖;rmmod 挂死 |
| **params 通路(params_poke)** | ✅ | 需按实际节点号(/dev/video13) |

**三条可复用的经验**:

1. **ISP 的 WB/AE 必须走 params 通路**,直写寄存器会被帧边界锁存覆盖——这是 RK ISP 的架构决定的,不是 bug。
2. **设备节点号会随 dts 变化**:`PARAMS_DEV` 这类硬编码要跟着改(我们的 L3 加了 OV8858 后节点整体后移)。
3. **算增益要会反推**:从 `U/V` 偏差用 BT.601 公式反推 R/B 增益,比盲试快得多;而且响应近似线性,一次外推就能收敛。

**还没关掉的问题**:rkaiq 的段错误(需要带符号调试,或换 SDK 里的 AIQ 版本);mini3a 的 AWB 判据(应改成"偏差超阈值才调整",而不是直接给 1.0)。

---

## 5. 收尾:修好 mini3a 的 AWB(让它真正闭环)

`mini3a` 的 AWB 之所以"报收敛但不生效",根因有两个,都在 `mini3a.c` 里:

1. **`awb_apply_params()` 硬编码 `/dev/video8`**(和 `params_poke` 同一个坑)。本系统 params 节点是 `/dev/video13`,open 失败 → 增益根本没下发。
2. **`awb_step()` 先更新 `cur_gain_*` 再调用 apply,且不看返回值** → 即使下发失败也返回"已调整",状态机以为自己调过了。

修法:

```c
/* 1) 节点自探测:依次试 13/8/9/14/15/16/17/10/11,第一个 G_FMT 成功的就是 params */
static int params_fd = -2;
if (params_fd == -2) {
    int cand[] = {13, 8, 9, 14, 15, 16, 17, 10, 11};
    for (...) { if (G_FMT 成功) { params_fd = cand[k]; printf(">>> AWB params 节点: /dev/video%d\n"); break; } }
}

/* 2) 只有真正下发成功才提交增益;并且每步限幅 ±20%,避免 u/v 接近 128 时振荡 */
if (awb_apply_params(gr, gb) == 0) { cur_gain_r = gr; cur_gain_b = gb; return 1; }
return 0;
```

**修复后实测**:

```
>>> AWB params 节点: /dev/video13 (sizeimage=10475)
f=10 Y=116 U=106 V=101 AE=RUNNING AWB=RUNNING
>>> AWB LOCKED at f=20 (u=128 v=128)
f=20 Y=110 U=128 V=128 AE=RUNNING AWB=LOCKED
...
>>> AE LOCKED at f=60 (y=111)
>>> 收官帧已保存 final.nv12  最终增益 g_r=0x133 g_b=0x133
```

落盘帧实测:**Y 111.0,U 128.0(+0.0),V 128.0(+0.0)** —— 完全中性,闭环成立。

> 注意增益值(0x133=1.2)比手动调时的 0x1EC 小,因为 AE 同时在改曝光/增益,两者的乘积才决定最终亮度与色度;这也说明**闭环里不能只盯增益绝对值,要看输出 U/V**。

---

## 6. rkaiq 段错误:定位到 AWB 预处理(用 gdb 抓到完整回溯)

在 gdb 里跑 `rkaiq_3A_server`,同时开流触发 3A:

```
Thread 9 "xc:GrpMsgThrd" received signal SIGSEGV
#0  interpolation(float const*, float const*, int, float, float*)   from /lib/librkaiq.so
#1  AwbPreProcV201(rk_aiq_awb_stat_res_v201_s, awb_contex_s*)       from /lib/librkaiq.so
#2  processing()
#3  RkCam::RkAiqAwbHandleInt::processing()
#4  RkCam::RkAiqCore::analyzeInternal(...)
#5  RkCam::RkAiqCore::groupAnalyze(...)
...
```

**崩溃点在 `AwbPreProcV201` 里对 AWB 统计做插值的 `interpolation()`**,而日志早在同一轮里报过:

```
ANALYZER:E:AWB stats invalid, ignore
```

即:**统计被判无效,却仍然送进 AWB 预处理**,`interpolation()` 拿到的是未初始化/空的点集 → 段错误。

**排除掉的两个可能**:

- 不是 IQ 文件损坏:三个变体(`.json` / `.bak` / `.bak2`)的 `main_scene` 都是合法 list、`main_scene_len=1` 与实际一致,JSON 都能正常解析
- 不是我们的 BSP 修复失效:同期 `params_poke` 通过同一节点成功下发并改变画面,说明 params 通路是好的

**结论**:这套 BSP 的 `librkaiq` 在"ISP AWB 统计无效"的路径上没做保护(空指针解引用),属于 **AIQ 库的健壮性缺陷**。可行的出路:

| 方案 | 说明 |
|---|---|
| **用 mini3a 做 AWB**(已完成) | 我们自己控制灰世界闭环,已验证 U/V → 128 |
| 换 AIQ 版本 | 板上有 `/usr/lib/librkaiq.so.bak-2022`,可试旧版是否不崩 |
| 关掉 IQ 里的 AWB | 让 rkaiq 只跑 AE,不碰 AWB 统计(但那样就没 AWB 了) |

> 这条也解释了为什么 L2-lite 当时最终选择"手写 3A":厂商 rkaiq 在这块 BSP 上不可靠,而 AWB 的核心只是**灰世界 + 两个增益的负反馈**,自己写反而可控。

---


```bash
# 1) 开流(必须流开着,params 才能提交)
./grab /dev/video5 200 /tmp/wb.raw 3840 2160 NV12 &

# 2) 施加白平衡增益(R=0x1EC, B=0x1F0)
./params_poke -g 0x1EC -b 0x1F0 -f 800

# 3) 校验:U/V 应接近 128
./ystat /dev/video5 3840 2160 1
```

---

## 7. 复现命令

```bash
# A) 手动施加白平衡(参数通路)
./grab /dev/video5 200 /tmp/wb.raw 3840 2160 NV12 &
./params_poke -g 0x1EC -b 0x1F0 -f 800

# B) mini3a 自动闭环(推荐)
cd /root/mini3a && ./mini3a -d /dev/video5 -w 1920 -h 1080
#   预期:AWB LOCKED at f≈20,最终 U/V ≈ 128,收官帧 /root/mini3a/final.nv12

# C) 复现 rkaiq 段错误(调试用)
gdb -batch -ex run -ex "bt 20" --args /usr/bin/rkaiq_3A_server &
./grab /dev/video5 180 /tmp/g.raw 3840 2160 NV12
```
