# 10 - L1-A Phase 3 任务书:第一帧

> 前置状态:Phase 2 已收官——`m00_b_imx415 1-001a` probe 通过,media 管线端到端挂载,默认格式 SGBRG10/3864x2192@15fps(见 09 复盘)。
> 本任务目标:**从 mainpath 抓出第一帧 NV12 数据,并用三重判读证明"这是传感器实时出的流",不是全零垃圾**。
> 严格模式:命令你自己敲、报错你自己贴,我只给地图和验收线。

## 0. 大局图:第一帧在整个管线里的位置

```
IMX415(3864x2192 RAW10, 2-lane)
   → csi2-dphy1(物理层接收)
   → rkisp-csi-subdev(选择 raw/isp 路径)
   → rkisp-isp-subdev(ISP:黑电平/去马赛克/白平衡/色彩空间……现在没跑 3A,用默认参数)
   → rkisp_mainpath = /dev/video0   ← 本任务主战场:出 NV12(已处理好的 YUV 图)
     rkisp_selfpath = /dev/video1   (备用小图路径,分辨率受限)
     rkisp_rawwr0   = /dev/video2   (RAW 直写路径,不过 ISP —— 进阶任务)
```

记住这张图:**video0 出来的已经是 ISP 处理后的 NV12**,不是 bayer 原始数据;video2 才是 RAW。两者抓谁都算"第一帧",但判读方式和文件大小完全不同——这是预测题要考的。

## 1. 前置知识(3 个,5 分钟)

1. **NV12**:YUV 4:2:0 半平面格式——先整面 Y(亮度),再 1/4 面 UV(色度) interleaved。一帧大小 = **宽 × 高 × 1.5 字节**。3840×2160 → 手算一下,后面验收要用。
2. **v4l2-ctl**:v4l-utils 工具包的瑞士军刀,直接对 /dev/video* 设节点做 ioctl:`--get-fmt`(查格式)、`--stream-mmap`(内核环形缓冲轮询抓流)、`--stream-count=N`(抓 N 帧)、`--stream-to=文件`(落盘)。media-ctl 你已经用过,它们是同一包的兄弟。
3. **为什么两帧 md5 要不同**:现在没跑 rkaiq(3A 守护进程),曝光是写死的固定值(`set exposure 2149` 那条日志)。画面内容不变则两帧近似相同;**对着镜头晃个手电再抓,md5 必须变**——变了才证明"活水"。

## 2. 执行前核对

- [ ] 线在 **J10**(上阶段末尾换过去的)
- [ ] 板子已重启且 dmesg 干净:`dmesg | grep -cE "Unexpected sensor id|No link"` 应输出 **0**
- [ ] 网络通:`ssh rkboard` 能进(或串口,本任务命令不多,串口也行)

## 3. 手术点(逐个做,每步先猜结果再看)

**手术点 1:确认工具与设备在位**
```bash
which v4l2-ctl media-ctl
ls /dev/video*
```
预期:v4l-utils 在(media-ctl 你一直在用);/dev/video0~8 十个节点。

**手术点 2:看 mainpath 的"合同条款"**(它接受什么格式、当前约定的 sizeimage)
```bash
v4l2-ctl -d /dev/video0 --get-fmt
v4l2-ctl -d /dev/video0 --list-formats-ext | head -40
```
记录两个值:当前 `Size Image`(字节)和 NV12 支持的尺寸列表。**先抄下来再动手**——这是你后面判读的基准线。

**手术点 3:设格式并抓一帧**
```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
  --stream-mmap --stream-count=1 --stream-to=/root/frame0.nv12
ls -l /root/frame0.nv12
```
判读:文件字节数 = 3840×2160×1.5 = **12,441,600**。对上了才算数。

**手术点 4:三重判读(核心!)**
```bash
md5sum /root/frame0.nv12
od -A d -t x1 /root/frame0.nv12 | head -3        # 看头部十六进制
grep -c $'\x00\x00\x00\x00' /root/frame0.nv12 2>&1 | head -1   # 粗看非全零(可选)
```
然后**开镜头盖、找个亮的东西(灯/手机屏幕)对着镜头晃**,连抓第二帧:
```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
  --stream-mmap --stream-count=1 --stream-to=/root/frame1.nv12
md5sum /root/frame0.nv12 /root/frame1.nv12
```
**验收铁证:两个 md5 不同。** 相同=全零死流(去翻车表)。

**手术点 5(进阶,选做):RAW 一帧不过 ISP**
```bash
v4l2-ctl -d /dev/video2 --get-fmt
v4l2-ctl -d /dev/video2 --set-fmt-video=width=3864,height=2192,pixelformat=BG10 \
  --stream-mmap --stream-count=1 --stream-to=/root/frame_raw.raw
ls -l /root/frame_raw.raw
```
注意宽高是 **3864×2192**(传感器全幅,含 dummy 像素)——和 ISP 输出的 3840×2160 不同,想想为什么(media-ctl 里的 crop (12,16) 就是答案)。

**手术点 6(观感,选做):把 NV12 拉回 PC 看图**
```bash
# PC 上(Git Bash):
scp rkboard:/root/frame0.nv12 D:/vm/frame0.nv12
```
用 [YUView](https://github.com/IENT/YUView/releases)(绿色单文件)打开:格式选 NV12,3840×2160。第一帧大概率**偏绿/偏暗**——正常,这正是 L2 装上 rkaiq+IQ 的理由,也是你 L2 对比报告的"处理前"素材。

## 4. 预测题(动手前先答,回复我后对答案)

1. 为什么主攻 video0 而不是 video1?(提示:selfpath 的能力边界)
2. 3840×2160 的 NV12 一帧多少字节?3864×2192 的 RAW10(每像素 2 字节容器)呢?
3. 没跑 rkaiq 时,ISP 用什么参数处理这帧图?画面会什么样?
4. 抓两帧 md5 完全相同,有哪几种可能原因?怎么区分?
5. `--stream-mmap` 里 mmap 指什么?如果换成 `--stream-user` 差在哪?

## 5. 验收标准(双证据,贴给我)

1. `ls -l` 两帧 + 两个 `md5sum` 输出(不同!)
2. `--get-fmt` 的 sizeimage 与文件字节数一致
3. (选做)YUView 截图一张 + RAW 帧大小

## 6. 常见翻车与自救

| 症状 | 大概率原因 | 自救 |
|------|-----------|------|
| 抓帧报 `EPIPE`(Broken pipe) | 设的宽高/格式链路不支持 | 回手术点 2 看 list-formats-ext,按它的来 |
| 抓帧卡住不动 | 链路没数据(dphy 没 stream on) | 确认用的 /dev/video0 而不是 subdev;重启再试 |
| 文件尺寸对但 md5 恒定、od 全 00 | 传感器没出流或镜头被挡 | 检查线在 J10、开镜头盖、对着亮处再抓 |
| `set-fmt-video` 不认 NV12 | 该节点只吃 RAW | 你可能拿错节点了,video0 才是 mainpath |
| 尺寸差一点点 | 3840 vs 3864 混了 | ISP 出口 3840×2160,RAW 是 3864×2192,各自有各自的数 |

## 7. 完成后

贴验收证据 → 我验收 → L1-A **全部收官**,L2(IQ/rkaiq 出彩图)任务书接力。本仓库 docs/ 会随里程碑持续更新。
