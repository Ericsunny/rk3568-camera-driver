# tools

## `iq_patch_smartrun.py`

给 Rockchip RK AIQ 的 IQ json 补上 AWB 的 `smartRun` 校准块。

**为什么需要**:RK3568 这套 BSP 里 `librkaiq.so` 是 2024-03 的 AIQ 6.0,而 `/etc/iqfiles`
下的 IQ 文件是 2022 年 schema(板上 36 个全部缺 `smartRun`)。库初始化 AWB 时,
`UpdateAwbCalibV2Para()` 发现 `smartRun.cfg` 的四组数组长度为 0,直接早退、不把数组
拷进策略,下一帧 `AwbPreProcV201()` 就拿 `NULL/NULL/len=0` 去调 `interpolation()` → 段错误:

```
AWB:E:lost smartRun para in json
Thread 9 "xc:GrpMsgThrd" received signal SIGSEGV
#0 interpolation(float const*, float const*, int, float, float*)
#1 AwbPreProcV201(...)
```

补上这 13 行 JSON 后,同一块板、同一个库:**600 帧 / 40.1 s 不崩**,AE 收敛到 Y≈47,
AWB 收敛到 U−1.5 / V−2.7。

> ⚠️ 这组数只是"单次能跑",**不代表 3A 可用**:后面的配对 A/B(`docs/21-L2-AWB.md` §8)显示
> rkaiq 三轮末态 Y = 181 / 51 / 75,AE 根本不可重复,日志里 `invalid main scene len`、
> `Can not get isp params buffer` 每帧都报。补丁的功劳只有一个:**不再崩**。

**用法**

```bash
# 先体检(不修改):打印 enable 与四个 _len
python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json --check

# 打补丁,默认 enable=0(只补字段,不改变 AWB 行为)
python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json

# 想真的打开 luma 自适应阈值
python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json --enable 1
```

脚本按文本插入(保留原文件排版,只增 13 行)、插入后重新 `json.loads` 校验、
重复执行幂等;遇到 `"autoExtPara"` 不唯一时拒绝写入,避免插错相机。

**注意**

- 补丁只改 IQ,不动库和内核;
- `enable` 开/关的 AWB 结果差异在测量噪声内(180 帧对照:U−1.5/V−2.7 vs U−1.6/V−2.8),
  所以默认 0 最保守;
- 换 IQ 文件(升级/换 sensor)后要重新打一次;
- 想回退:删掉 `autoExtPara` 里的 `smartRun` 对象即可(或从备份还原 IQ)。

## `compat_test.c` —— 32/64 位 ioctl 对照

同一段代码编两份(armhf / aarch64),各自对一个 sensor subdev 发
`RKMODULE_GET_MODULE_INFO`,`main()` 只做一件事:打印 ioctl 返回值。

```bash
INC=$HOME/rk3568/RK_SDK/kernel/usr/include          # 注意 SDK 是嵌套路径
# 32 位静态
arm-linux-gnueabihf-gcc -static -O2 -I$INC compat_test.c -o compat_test32
# 64 位静态
aarch64-rockchip-linux-gnu-gcc -static -O2 -I$INC compat_test.c -o compat_test64
```

判据:

| | 未实现 `.compat_ioctl32` | 已实现 |
|---|---|---|
| `compat_test64` | OK | OK |
| `compat_test32` | `-1 errno=515`(= `-ENOIOCTLCMD`,VFS 找不到兼容入口) | OK |

注意 `#include <linux/videodev2.h>` 必须在 `rk-camera-module.h` 之前,
否则 `BASE_VIDIOC_PRIVATE` 未定义、`RKMODULE_*` 全部编译不过。

## `ab_awb.py` —— rkaiq AWB vs mini3a AWB 同场景对比

三条路径、同一分辨率、同一场景、同一时长,分别抓帧统计:

| arm | 做法 |
|---|---|
| baseline | 不跑任何 3A,DUT 直接出图(参考系) |
| rkaiq | 起 `rkaiq_3A_server`,由 librkaiq 闭环 |
| mini3a | 跑 `mini3a/mini3a`(自写的小 3A) |

每条 arm 输出:末帧平均 Y、U/V 相对 128 的偏移、收敛所用帧号、最后 5 个采样点的
抖动(峰峰值)。抓帧用 `grab`,统计只用原始 RAW10 的亮度/色度均值,不做 ISP 处理。

## `step1_install.sh` / `step2_verify.sh` —— 换驱动的正确姿势

> **前提:本 BSP 里 `CONFIG_VIDEO_OV8858_SM=y`(内建)**。`/proc/modules` 是空的,
> `/sys/module/ov8858_sm/` 只有 `parameters`/`uevent`、没有 `sections`;`lsmod` 什么都不打印。
> 往 `/lib/modules/$(uname -r)/extra/` 装 `.ko` + `depmod` + `modprobe` **全部无效**
> (`modprobe` 只回 `module is already loaded`),板上跑的还是编进 Image 的旧代码。
> 这就是"改了源码但 `enum_frame_size` 一直 `-ENOTTY`"的真因。

正确姿势是**重编 Image 并刷 `/boot`**:

```bash
# VM:驱动源码 → 内核树 → Image
cp ~/work/ov8858_sm/ov8858_sm.c ~/rk3568/RK_SDK/kernel/drivers/media/i2c/ov8858_sm.c
cd ~/rk3568/RK_SDK/kernel
TC=$(ls -d ../prebuilts/gcc/linux-x86/aarch64/gcc-buildroot-9.3.0-2020.03-*/ | head -1)
LD_LIBRARY_PATH=${TC}lib make ARCH=arm64 \
  CROSS_COMPILE=${TC}bin/aarch64-rockchip-linux-gnu- -j6 Image

# 刷机(Windows/VM 直接 scp;/boot 是 ext2 124M,原本 100% 满,先腾地方)
scp arch/arm64/boot/Image root@192.168.10.2:/boot/Image-4.19.232
ssh root@192.168.10.2 'sync && reboot'
```

`step1_install.sh` / `step2_verify.sh` 保留模块路线写法(只有把配置改成 `=m` 才有用);
`step2_verify.sh` 里的检查项(enum / compat / A/B)在内建路线下同样适用——把"模块来源"那一步换成
`grep ov8858_sm /proc/kallsyms`,看到 `t ov8858_sm_enum_frame_size` 就说明新 Image 生效了。

**不要热 `rmmod ov8858_sm`**:rkcif 仍把这个 sensor subdev 挂在它的 async notifier 上,
`v4l2_async_unregister_subdev()` 拆 media 图时和 rkcif 撞成死锁,整机 hang(串口只剩
tty 回显、网络全断、SysRq 不应答,只能断电)。这条教训在模块路线下依然成立。

## `tools/ab/` —— 同场景 A/B(配对法)

```bash
# 板上:拷到 /root/ 下直接跑,两轮 A/B 交替,间隔 <1 min
bash /root/pair_ab.sh
```

| 脚本 | 作用 |
|---|---|
| `pair_ab.sh` | 配对跑 A/B 两轮:每臂前重置 `exposure=300,analogue_gain=4`,A 起 `rkaiq_3A_server` 抓 180 帧、B 跑 `mini3a -n 400`,结果写 `/root/ab/` |
| `run_ab.sh` | 单轮 A/B(顺序执行,不配对) |
| `run_b.sh` / `run_a0.sh` | 只跑 mini3a / 只跑无 3A 基线 |
| `cpu_ab.sh` | 量 rkaiq daemon 与 mini3a 的 CPU(`/proc/<pid>/stat` ticks ÷ 帧数) |
| `iq_cmp.sh` | 原版 IQ vs 补丁版 IQ 的启动日志对照(证明 `invalid main scene len` 是原有的) |

数据落点:`/root/ab/{A.nv12,B.final.nv12,A_grab.log,B_mini3a.log,rkaiq.log}`。

结果见 `docs/21-L2-AWB.md` §8:mini3a 两轮逐位可重复(U/V 精确 128、AE 稳定 Y≈113),
rkaiq 三轮末态 Y = 181 / 51 / 75(3.6 倍漂移),CPU 却只有 mini3a 的 1/13。

**那个 1/13 是坏 daemon 的数字**——修好版本后见 §9 与下一节。

## `install_rkaiq_2022.sh` —— 把 rkaiq 换回与 BSP 配套的 2022 三件套

```bash
scp tools/install_rkaiq_2022.sh root@192.168.10.2:/root/
ssh root@192.168.10.2 'sed -i "s/\r$//" /root/install_rkaiq_2022.sh; bash /root/install_rkaiq_2022.sh'

bash /root/install_rkaiq_2022.sh --check     # 只看版本
bash /root/install_rkaiq_2022.sh --rollback  # 回滚到 2024 版
```

三条 md5 闸门(必须先通过才动手):

| 文件 | 2022 版 md5 | 2024 版 md5 |
|---|---|---|
| `librkaiq.so` | `654dc84e89edc0aae9ee5f646732ba62` | `be51ce86c4e5fdc9d702ead2311d89a6` |
| `rkaiq_3A_server` | `bc67a0fc6ca1221923132c65c3381aed` | `7640afb5b9c2133a865d4f93f3d9789d` |
| IMX415 IQ | `0666ba18ce1e4db30cddc92810587fb1`(331401 B) | `82e3adfe…`(788677 B，打过 smartRun) |

换完要 **reboot**:rkaiq 被杀后 ISP 可能停在 stream 未停的状态,紧接着开 `/dev/video5` 拿不到帧。
装好后 2022 daemon 的 AE/AWB 才真的闭环(实测 15 ms/帧,mini3a 36.3 ms/帧)。


## `hdr_demo.c` —— 板端 HDR 多帧融合(路线 A)

一条命令完成「设曝光 → 抓 3 档 → 线性域加权融合 → 存 NV12」,并打出各阶段耗时。
算法与 `hdr_fuse.py` 相同(曝光适中度 × 局部对比度,可分离盒式平滑),但用 C 重写、无需 numpy。

```bash
# 编译(VM 上交叉编译)
TC=$(ls -d ~/rk3568/RK_SDK/prebuilts/gcc/linux-x86/aarch64/gcc-buildroot-*/ | head -1)
export LD_LIBRARY_PATH=${TC}lib
${TC}bin/aarch64-rockchip-linux-gnu-gcc -O2 -std=gnu99 -o hdr_demo hdr_demo.c -lm

# 板端运行(前提:本次启动后从未起过 3A,见 docs/25 §7)
/root/hdr_demo -d /dev/video5 -s /dev/v4l-subdev6 -w 3840 -h 2160 \
               -e 60,300,1400 -n 6 -o /root/hdr2/fused_4k.nv12 -k
```

实测耗时(RK3568):4K 融合净 **1285 ms/帧**、1080p **346 ms/帧**。
效果(4K 同场景):高光溢出 5.74% → **0.00%**,暗部 73.5% → **7.07%**。
`-k` 会把每档输入也存成 `out.nv12.e<曝光>.nv12`,方便做前后对比。
## 27. 路线 B:2-lane DOL X2 的两个补丁 + 切换工具

`patch_imx415_hdr.py` / `patch_imx415_hdr_hts.py` / `hdr_cfg.c` 用于 docs/26 的实验。

```bash
# 在 VM 的 SDK 内核树上注入 HDR_X2 模式(幂等,重复跑会跳过)
python3 tools/patch_imx415_hdr.py
# 可选:把新表 HTS 翻倍(排除行速率因素)
python3 tools/patch_imx415_hdr_hts.py
# 重编 Image 后刷 /boot(见 docs/17 §10)
```

```bash
# 板上切换模式
/root/hdr_cfg /dev/v4l-subdev6 5   # HDR_X2
/root/hdr_cfg /dev/v4l-subdev6 0   # NO_HDR(切回后需再 S_FMT 回 3840x2160)
```

实测结果见 docs/26:传感器侧切换成功,CSI/ISP 侧 MIPI overflow 收不到帧,**未通**。
