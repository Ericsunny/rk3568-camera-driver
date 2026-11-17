# V4L2 采集缓冲到 MPP 的局部 DMA-BUF 评估

2026-11-13 在 RK3568 板端测试。结论：两种相机的固定帧导入 MPP 和小规模输出对照通过，隔离的编码微基准显示较低 CPU 时间；**尚不能据此替换现役服务**。连续采集的输入归还时机、OSD、人体检测、RTSP、事件端到端与故障恢复均未由 DMA-BUF 候选实现验证。

## 环境和范围

- 板端 Linux 4.19.232 #62；测试前板钟为 2024-06-18，已按控制机时间校正。[内核](kernel.txt) · [校时后时间](board-time.txt)。
- 本地工作树 HEAD 为 `3a9f9c09becffe540da2a5dd433802fe4d7a8456`，另有未提交修改；单独编译的复制基线 evrec SHA-256 为 `620f67a160df04d238e57cc8cd26e2bdf32328a611329f9848714a46b7b99509`，与板端既有 `/root/camera-integration-20261110-app/evrec` 一致。[源码与二进制指纹](frozen.sha256) · [编译输出](build-copy.log)。
- IMX415 对应 `/dev/video5`；OV8858 对应 `/dev/video14`。两路分别测试，3A 全程保持运行，没有部署新程序或修改默认镜像。
- 运行目录为板端 `/root/zc-eval-20261113-Y7s5zu`；采集录像和图像仅留在该目录，**不包含在本证据目录**。因此这里的哈希和计数可以核对记录，但不能仅凭本仓库重新检视现场画面。
- 各采集节点在测试后恢复原格式；两节点恢复前后的 G_FMT 文本一致，3A 为 active，测试进程已退出。[恢复记录](restore.json)。

## 复制基线：手动事件

只测复制版本，关闭检测、OSD 和 RTSP，触发后正常收尾：

| 相机 | 配置 | 进程退出 | 解码结果 | 记录 |
|---|---|---:|---|---|
| IMX415 | 1920×1080，15 fps | 0 | ffprobe 计数 85 帧；ffmpeg 退出 0、stderr 为空 | [进程结果](baseline-imx-r1/result.json) · [日志](baseline-imx-r1/run.log) · [ffprobe](baseline-imx-r1/probe.txt) |
| OV8858 | 1280×960，30 fps | 0 | ffmpeg `-vsync 0` 解码 174 帧，退出 0、stderr 为空 | [进程结果](baseline-ov-r1/result.json) · [日志](baseline-ov-r1/run.log) · [解码记录](baseline-ov-r1/verify.json) |

首次 IMX 基线命令因板上没有 `/usr/bin/time`，录像程序未启动，记录保留在 [baseline-imx](baseline-imx/status.txt)。后续使用 [Python 控制器](controller.py) 捕获子进程资源统计并在运行六秒后发送 SIGUSR1；不同相机的目标尺寸、帧率和命令在进程结果中保存。基线不证明 DMA-BUF 事件链路可用。

## 固定三帧：同一输入的复制与导入对照

采集三帧后暂不 QBUF，让两个编码上下文依次使用相同的、不会被摄像头覆盖的输入。复制路径向 MPP 自有 buffer 复制，候选路径将 V4L2 MMAP buffer 经 EXPBUF 导出并通过 `mpp_buffer_import` 导入；两个编码上下文销毁后才关闭导出 fd 和采集设备。未启用检测或 OSD，不是实时循环采集测试。

| 相机 | 配置 | DMA-BUF 导入 | 结果 |
|---|---|---|---|
| IMX415 | 1920×1080 | 三个 fd 导入返回 0 | 两种路径各完整解码 3 帧，逐帧原始图像哈希一致：[日志](fixed-imx-r1/run.log) · [对照](fixed-imx-r1/compare.json) |
| OV8858 | 1280×960 | 三个 fd 导入返回 0 | 两种路径各完整解码 3 帧，逐帧原始图像哈希一致：[日志](fixed-ov-r1/run.log) · [对照](fixed-ov-r1/compare.json) |

IMX 测试用 [fixed_probe.c](fixed_probe.c)，OV 测试用可指定尺寸和帧率的 [fixed_probe_v2.c](fixed_probe_v2.c)，两者含同一工作树 evrec 实现。这里没有在编码进行中重新把共享 buffer 交给摄像头，因此不验证持续复用，也不能据此推断 `encode_put_frame` 返回即安全归还。

## 固定单帧的局部性能微基准

每路采集一帧后始终持有，不重新 QBUF；同一帧每轮重复编码 60 次，顺序为 copy、DMA-BUF、DMA-BUF、copy、copy、DMA-BUF，分别运行三轮。均关闭检测、OSD 和 RTSP。指标是该探针编码循环的**进程 CPU 时间**，包含复制、编码调用和输出写入，取每组中位数；不是完整服务 CPU 占用，也不是摄像头实时帧率。

| 相机与输入 | copy CPU ms/帧中位数 | DMA-BUF CPU ms/帧中位数 | 本次微基准相对下降 |
|---|---:|---:|---:|
| IMX415 1920×1080 | 3.53 | 1.73 | 约 51.0% |
| OV8858 1280×960 | 2.16 | 1.02 | 约 52.8% |

IMX 每轮 copy 显式复制 186,624,000 B；OV 每轮复制 110,592,000 B；DMA-BUF 路径此处记录的 CPU 显式输入复制为 0，不代表设备没有内存读写。每个相机的六个 H.264 输出 SHA-256 相同；各取一份用 `ffmpeg -vsync 0` 解码，均得到 60 帧且 stderr 为空。[IMX 原始测量](perf-imx-r1/run.log) · [IMX 计算](perf-imx-r1/summary.json) · [IMX 解码](perf-imx-r1/decode-vsync0-summary.json) · [OV 原始测量](perf-ov-r1/run.log) · [OV 计算](perf-ov-r1/summary.json) · [OV 解码](perf-ov-r1/decode-summary.json) · [探针源码](perf_probe.c)。

IMX 首次用 ffmpeg 默认帧同步得到的 framemd5 只有 1 帧；保留 [原记录](perf-imx-r1/decode-summary.json)。随后 ffprobe 计数为 60，用 `-vsync 0` 重新解码得到 60 帧；因此以明确禁用同步丢帧的结果核对，不能抹去初次差异。

固定单帧不覆盖自然画面变化、真实采集速度、NPU/RGA 并发、OSD、网络、磁盘事件链路和共享 buffer 循环回收；这些百分比不能写成当前主服务的 CPU 改善或整机加速成绩。

## 输入 buffer 归还时机：未解决

在复制基线的前八帧编码输出包上探查 `KEY_INPUT_FRAME`，`mpp_meta_get_frame` 均返回 -1、未取得对应输入帧：[探针变更](evrec.meta.patch) · [日志](meta-imx-r1/run.log)。另一组不使用摄像头、以合成帧调用 MPP 任务接口的实验中，输入/输出任务调用成功并得到非空 packet，但同样未取到对应输入帧：[方法](task-synthetic-r1/plan.txt) · [日志](task-synthetic-r1/run.log) · [源码](task_probe.c)。这只说明**本次两种探针没有取得该标识**，不证明整套 MPP API 完全没有安全完成机制。

板端头文件对异步任务的说明不支持把 enqueue 成功直接当作任务完成。后续必须依据匹配板端库的源码或契约，确认摄像头 buffer 何时可复用；若无法确认，就不能将共享 buffer 提前 QBUF。现役启动脚本启用 CPU OSD，固定帧候选关闭 OSD；直接替换还会改变功能。未实现、未部署候选服务。

## 可复核资料与风险记录

- 每组的预设参数保存在相应 `plan.txt`，运行日志、返回码、对照文件和每份文件的 SHA-256 见本目录；[导出文件清单](manifest.sha256.json)。原始视频不入库。
- 四组固定帧测试的内核日志增量保存在对应目录下的 `dmesg-delta.json`。检测脚本在增量中搜索 error/fail/oops/warn/timeout 等词未命中；这不是完整可靠性证明。IMX 复制基线结束后的后续解码阶段内核日志出现两条 `rga2: unknown ioctl cmd!`，无法仅凭这次合并快照把每条精确归属于某个进程。
- 实验代码仅位于本证据目录用于复现，未接入现役程序或 CI；导入、编码和输入释放语义仍需在目标 BSP 及实际负载下单独验证。
- 编译方式：将本目录探针与 `frozen.sha256` 对应的 `evrec.c`、`rtsp_hub.c`、`rtsp_hub.h` 放在同一独立目录，在板端使用 `gcc -O2 -Wall -pthread -DHAVE_RGA -DHAVE_RKNN -I/usr/include/rga -I/root/npu/3rdparty/rknpu2/include -o PROBE PROBE.c rtsp_hub.c -lrockchip_mpp -lrga -lrknnrt -lm`；执行前需核对 3A、设备占用、环境与测试场景。不要覆盖现役文件。
