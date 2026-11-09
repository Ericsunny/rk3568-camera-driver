# mini3a：IMX415 软件 AE/AWB 实验

`mini3a.c` 从 IMX415 的 NV12 图像统计亮度和色度，通过传感器 V4L2 controls 调整曝光/增益，通过 Rockchip ISP params 节点提交红/蓝数字增益。它是 `rkaiq` 之外的简化 AE/AWB 实验，没有实现自动对焦，也不代表完整 ISP Tuning。

当前统计、控制和收敛循环均有实现。旧版残留的 TODO 注释曾使文档将其误判为纯骨架；本轮补齐的是错误传播、行对齐、线程退出和结果文件检查。历史[白平衡实验](../docs/image/awb.md)仍对应当时的程序和环境，新版的主机检查不能代替板端闭环或画质验证。

2026-11-08 的[真实设备验证](../evidence/2026-11-08-event-service/README.md)发现并修复了 ISP21 params 缓冲分配大小不匹配；#57 上 AE 调整后亮度达到目标。该次图像的 UV 恒为128，因此程序打印 AWB LOCKED 只表示均值判据满足，不能证明白平衡效果。本版将 mini3a 用于 AE 诊断与参数接口实验，主视频继续使用 rkaiq。

## 构建

需要 Linux、C11 编译器及与目标 BSP 匹配的 vendor UAPI。例如在编译 VM：

```sh
make -C mini3a BSP_UAPI=/path/to/kernel/include/uapi
```

板端已安装匹配的 `rkisp21-config.h`、`rkisp2-config.h` 等头文件时可以直接 `make -C mini3a`；交叉编译可覆盖 `CC`。不需要 RGA/RKNN 库。完整构建和主机测试见[本轮记录](../evidence/2026-11-08-mini3a-host/README.md)。

## 运行与结果

**仅在已确认没有其他 3A 程序控制同一链路的测试环境运行。** 不要直接停止现役 `rkaiq` 后立即运行；历史记录中这种操作可能使 ISP 链路卡住。服务变更、重启和恢复需要按现场状态另行批准，见[复现约束](../docs/acceptance/reproduction.md)。

节点必须通过媒体拓扑核对，以下节点号只是历史环境示例：

```sh
./mini3a/mini3a -d /dev/video5 -s /dev/v4l-subdev6 -p /dev/video13 \
  -w 3840 -h 2160 -n 300 -o /path/to/new-run/final.nv12
```

- `-d`：单平面 NV12 的 multiplanar 采集节点；`-s`：IMX415 传感器控制节点；`-p`：该链路的 ISP params 节点。程序检查格式、尺寸及 params 结构大小，拒绝 RAW10 或不匹配的 UAPI。
- `-o`：必须是不存在的新文件，不覆盖已有文件。成功保存的 NV12 去除行 padding，尺寸为实际宽×高×1.5。写入或关闭失败返回错误，残留文件可能不完整。
- `-n`：正整数帧数上限，默认 300；到上限仍未收敛时保存当时一帧供分析，退出码为 2。
- 退出码 0 只表示达到本程序的亮度目标带与简单色度判据、文件保存及清理成功；不代表画质优秀或长期稳定。整帧均值控制存在高光过曝等局限；U/V 接近中性不是通用场景的颜色准确度证明。
- 设备、控制提交、映射、线程或文件错误返回 1。SIGINT/SIGTERM 返回 128+信号号。控制提交成功也不等于效果验证，仍须观察后续图像。

## 其他源码的位置

- `realdetect.c` 和 `Makefile.yolov8` 保留早期模型工具练习，其中 RGB 转换及推理主循环尚未完成，**不作为当前检测运行入口**。检测入口为 [`evrec --detect`](../tools/evrec.c)；采集队列和输入 native 实验见 [`camd`](../tools/camd.c)及[历史零拷贝实验](../zerocopy/README.md)。
- `isp_wb.c` 保留直接写 ISP 寄存器的旧实验；当前 `mini3a` 使用 params 节点，不需要加载该模块。`params_poke.c` 是旧接口探针。
- OV8858 在本项目走 CIF RAW10，不属于这条 ISP AE/AWB 路径；RAW 保存、预览和双路实验见[双摄记录](../docs/pipeline/dual-camera.md)。

## 不接板的检查

在 Linux 主机运行：

```sh
python3 tools/tests/test_mini3a_host.py
MINI3A_SANITIZE=1 python3 tools/tests/test_mini3a_host.py
BSP_UAPI=/path/to/kernel/include/uapi python3 tools/tests/test_mini3a_host.py
```

前两项提取实际源码函数，以替身 I/O 检查统计、控制错误、文件写入和线程队列；指定 `BSP_UAPI` 后额外编译完整程序，用模拟设备检查退出状态和失败清理。测试中的图像、参数结构替身均不是板端证据，也不验证算法效果。完整程序仍须使用实际 BSP 头文件另行构建并做板测。
