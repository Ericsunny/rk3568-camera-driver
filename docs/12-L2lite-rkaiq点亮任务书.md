# 12 - L2-lite 任务书:rkaiq + IQ,让第一张"正常色"的图出现

> **格式说明(2026-10-08 起永久生效)**:任务书开头 = ①任务目标(本阶段要掌握的知识点)②资料链接(中英)③手术点 ④预测题。
> 背景:L1-A 已出 NV12 但是"生图"(偏绿偏暗,3A 闭环断);本阶段让 rkaiq 接管 3A,出正常色图。
> 侦察情报:课程代码(101~105 讲)**不依赖 rkaiq**(纯 V4L2 裸抓)——3A 路径是全新领地,也是真正的"活"。

## ① 任务目标(学完这个阶段,你应该能回答/做到)

1. **画出 3A 闭环的完整回路**:sensor 出帧 → ISP 硬件统计(video7 的 stats)→ **用户态 rkaiq 读取并计算** → 写回参数(video8 的 params)→ ISP 硬件应用 → 影响下一帧。"用户态算法 + 硬件统计/应用"分工是 Rockchip ISP 架构核心题
2. **说清 IQ 文件是什么、为什么必需**:同一颗 IMX415,不同模组厂的镜头(CRA/滤光片/封装)不同 → 黑电平/镜头阴影/色彩响应不同 → 每个模组要标定自己的 IQ。出厂 rootfs 里 `imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json` 就是为野火这块模组标定的正版
3. **把"生图症状"映射到 ISP 模块**:偏绿/偏品红←AWB/CCM;四角暗←LSC;黑位发灰←BLC;噪点裸奔←3DNR/BayerNR;亮度死板←AE。症状→模块的对应关系是 ISP 调试岗的日常语言
4. **掌握 rkaiq 的初始化模型**:`rk_aiq_uapi2_sysctl_init(iqfiles路径, sensor entity名, …)` → `start` → 库自己找到 media 拓扑、接管 stats/params 节点;应用层从此只管抓帧
5. **做完一次严格对照实验**:同一场景、同一参数,生图 vs IQ 图各存一帧——这张对比图就是你博客/简历的素材

## ② 资料链接

| 资料 | 语言 | 说明 |
|------|------|------|
| librkaiq 源码仓 https://github.com/airockchip/librkaiq | 英 | API 头文件在 `include/uAPI2/`,看 `rk_aiq_uapi2_sysctl.h` 即可;README 有最小初始化示例 |
| Rockchip ISP2X 文档全套 | 中 | **E:\RK3568-iTOP-迅为\04官网文档资料\**(下次挂硬盘我给你定位到具体 PDF;《RKISP Developer Guide》讲 mainpath/selfpath/3A 架构) |
| 野火摄像头章节 https://doc.embedfire.com/linux/rk356x/quick_start/zh/latest/quick_start/camera/camera.html | 中 | 设备树插件使能相机的方式,可对照我们 dtb 直改的方式想"两种流派" |
| 出厂 IQ JSON(活教材) | - | 板上 `/etc/iqfiles/imx415_*.json`——重点看 `aWB`/`aE`/`blc`/`lsc`/`ccm` 段,和上面症状映射表对号入座 |
| V4L2 ISP 统计/参数概念 | 英 | https://www.kernel.org/doc/html/latest/driver-api/media/v4l2-subdev.html(轻读,有个概念) |

## ③ 手术点

**手术点 0:开机恢复 + 侦察(决定后面走哪条分支)**

板子上电,串口进系统,把 IP 配回来(运行时配置断电即失):
```bash
ifconfig eth0 192.168.10.2 netmask 255.255.255.0 up
```
PC 侧确认 `ssh rkboard` 能进。然后**你来做组件侦察**(命令自己敲,贴结果给我):
```bash
ls -lh /usr/lib/librkaiq* 2>/dev/null; ls /usr/bin | grep -iE 'aiq|rkipc' ; ls /etc/iqfiles/ | head -5
```
**分支决策树(侦察结果对号)**:
- **A. 有 librkaiq.so + rkipc/aiq 服务脚本** → 手术点 2 直接启动服务,最顺
- **B. 只有 librkaiq.so,没有服务** → 我们交叉编译一个 ~40 行的最小 3A 守护 demo(调 sysctl_init+start),我来出 demo 骨架任务书,你编译部署——这是最优学习路径
- **C. 什么都没有** → 从 librkaiq 仓库拉预编译 so(经镜像)+ 头文件,同样走 B 的路

**手术点 1:基线留存(生图证据固定)**

PC 侧把上次的两帧拷回来(⚠️ 这条在 **PC Git Bash** 跑,不是板子):
```bash
scp rkboard:/root/frame0.nv12 D:/vm/frame0.nv12
```
YUView 打开(NV12 / 3840×2160),截图存 `D:\kb\10-projects\RK3568相机驱动项目\assets\`(没有 assets 目录就建)——这就是 L2 对比报告的"处理前"。

**手术点 2:让 3A 跑起来**(按手术点 0 的分支执行,具体命令等侦察结果)

**手术点 3:同场景对照抓帧**
```bash
# 与 frame0 同一场景、同一角度(对着你桌上某物,别挪)
v4l2-ctl -d /dev/video0 --set-fmt-video=width=3840,height=2160,pixelformat=NV12 --stream-mmap --stream-count=1 --stream-to=/root/frame_iq.nv12
md5sum /root/frame0.nv12 /root/frame_iq.nv12   # 不同是必然(曝光变了)
ls -l /root/frame_iq.nv12
```
scp 拷回,YUView 并排对比 frame0 vs frame_iq,截图。

**验收(双证据)**:①对比截图(色彩/亮度/四角,肉眼可见改善);②dmesg 里 rkaiq 的启动日志(或 demo 进程存活证据)。**外加预测题全交。**

## ④ 预测题(含两笔上阶段欠债)

1. **(还债·重答)** 上阶段你答 video0/video1 之别时写"驱动写失败不上报 vb2 桥,sensor 按老状态出货"——这句其实描述的是另一个现象。先用一句话说清你当时想描述什么(提示:s_stream 首写失败但流未断,那 sensor 到底在不在按新状态出货?读 09 复盘 §2 的 dmesg 再答);然后正式答:**mainpath / selfpath / rawwr0-3 三类节点的分工各是什么,4K 第一帧为什么必须 mainpath**
2. rkaiq 跑起来后,video7(statistics)和 video8(params)会被**谁**打开?你的抓帧程序还要不要碰它们?为什么?(提示:硬件统计的消费者)
3. IQ JSON 里,改 `blc`(黑电平补偿)会改变画面什么?改 `ccm`(色彩矩阵)呢?各对应生图的哪个症状?
4. **(还债)** 抓帧得到全 0x00 文件的**四类可能原因**+每种用什么手段区分(dmesg 关键字/od 看什么/多帧互比)
5. 同一块 IMX415 模组,换一个 CRA 不同的镜头,要不要重标定/换 IQ?为什么?(提示:LSC 标定是对着什么标出来的)

## 预计耗时

手术点 0-1:30 分钟;手术点 2 视分支 A/B/C:30 分钟(A)~ 半天(B/C);手术点 3:30 分钟。预测题建议穿插做。
