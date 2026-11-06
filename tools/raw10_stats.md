# RAW10 离线统计(合成数据验证,真实采集待验证)

`raw10_stats.py` 读取**已有的单帧文件**,计算 Bayer 各通道的原始数值(DN)统计。它不访问设备、不改输入文件、不做 ISP 或画质自动判级。适合作为受控暗场/固定场景实验的计算环节;真实 sensor 上的黑电平、噪声和坏点结论仍需独立采集与验证。

## 先确认布局

- `--packing raw10p`:每 4 个样本 5 字节,前 4 字节为各样本高 8 位,第 5 字节从低到高依次保存这 4 个样本的低 2 位。宽度必须为 4 的倍数。依据 [Linux 4.19 packed Bayer 说明](https://www.kernel.org/doc/html/v4.19/media/uapi/v4l/pixfmt-srggb10p.html),并与 VM 中对应内核文档对照。
- `--packing u16le-lsb`:每样本 16 位小端,有效值在低 10 位,高 6 位须为零;依据 [Linux 4.19 expanded Bayer 说明](https://www.kernel.org/doc/html/v4.19/media/uapi/v4l/pixfmt-srggb10.html)。
- `--bayer` 是**输入帧左上角**排列,不是自动推断的 sensor 原生排列;裁剪、镜像、翻转均可能改变相位。`--stride` 是每行实际字节数,必须显式给出,不自动猜测 padding。

历史资料将板端某些输出记为 `BG10` 和 compact RAW10;标准 V4L2 的 `BG10` 与标准 packed `pBAA` 是不同 FourCC,不能单凭名字认定该板文件的布局。应保存实际 `G_FMT` 的 FourCC、`bytesperline`、`sizeimage`、驱动版本与捕获工具的写盘方式,再以受控样本确认。若 vendor 布局与上面两种不同,本工具不适用,不得强行选择后解读统计。

每个输入文件必须恰好是 `stride × height` 字节的一帧,不同输入须是不同文件/inode;不接受多帧拼接、头部或尾部元数据。每行 padding 会纳入文件 SHA-256,但不计入像素统计。`u16le-lsb` 若在被评估行遇到超过 1023 的值则报错。输出默认 stdout;`--output` 只创建新文件,已有文件不会覆盖。创建后写盘失败可能留下不完整新文件,退出非零时不可把它视为结果。

## 指标和解释条件

| 输出字段 | 定义 | 边界 |
|---|---|---|
| `mean_dn` | ROI 内该通道所有像素、所有帧的算术平均 | 遮光且固定曝光/模拟增益/温度等条件下,可作为暗场均值候选;不自动等于传感器标定黑电平 |
| `pooled_std_dn` | 同一组样本的总体标准差,分母为样本数 | 混合空间差异和跨帧差异,不能单独解释为时域噪声 |
| `temporal_rms_dn` | 对每个像素以帧间样本方差(分母 `N-1`)计算,对同通道像素方差求平均后开方 | 至少两帧;须固定场景、曝光/增益/温度与像素位置。它是 DN 域帧间波动,不能单独称为电子读出噪声 |
| `clipped_samples_at_1023` | 值恰为 RAW10 最大码值的样本数 | 不是自动的曝光或画质合格判定 |
| `above_threshold_pixels` | 当指定 `--hot-threshold` 时,帧间均值严格大于该 DN 的像素数 | 仅是候选点计数;亮场、非均匀遮光、固定图案或错误布局都可能触发,不能直接宣称坏点数 |

`R`、`B`、`Gr`、`Gb` 分开报告;`Gr/Gb` 分别是与 R/B 同行的绿像素。ROI 的坐标基于完整输入帧,因此奇数起点不重置 Bayer 相位。缺失通道的均值和标准差为 `null`;单帧时所有 `temporal_rms_dn` 为 `null`。工具不自动减黑电平、不做 demosaic、颜色矩阵、校正或时空滤波。

## 用法与测试

在 Linux/VM 仓库根目录,对已经合法取得、条件一致的单帧文件示例(分辨率、ROI、阈值和路径只是占位示意,必须按实际记录替换):

```sh
python3 tools/raw10_stats.py --packing raw10p --bayer BGGR \
  --width 4 --height 2 --stride 5 --roi 0,0,4,2 \
  --hot-threshold 100 --output new-report.json frame-1.raw frame-2.raw
```

JSON 保存每帧路径、SHA-256、布局、尺寸、ROI、阈值、帧数(帧列表长度)及各通道统计。采集的曝光、增益、遮光、温度、sensor 模式、设备树/驱动与 `G_FMT` 应另存采集清单;程序不会替你验证这些条件。避免把用户路径等敏感内容直接提交,证据需脱敏审查后新增。

无需设备的验证:

```sh
python3 tools/tests/test_raw10_stats.py
```

测试仅生成合成像素与 padding,覆盖已知低位字节序、四种 Bayer 相位、奇数 ROI、多帧时域方差、饱和码、两种存储布局、异常长度和禁止覆盖;[VM 测试日志及源码指纹](../evidence/2026-11-05-RAW10合成测试/README.md)已留存。**本次尚未在真实 OV8858 RAW 帧上运行或验证格式**,不代表获得了真实画质、黑电平、噪声或坏点结果。板上采集须先确认设备状态、授权及证据记录方式。
