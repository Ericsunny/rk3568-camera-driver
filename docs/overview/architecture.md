# 入口相机架构与数据所有权

交付入口为 `camera_demo imx415-isp entrance`。一颗 IMX415 的 RAW 数据经 CSI-2 送入 ISP。rkaiq 依据统计信息更新传感器与 ISP 参数。启动器通过 Media Controller 解析已启用的实体、pad 和 link，得到同一传感器的 mainpath、selfpath 视频节点。具体节点编号随 BSP 而变。

| 边界 | 输入、输出与生命周期 | 源码 |
|---|---|---|
| 启动与监督 | 启动器核对配置、模型哈希、3A 进程、主路占用和端口，保存实际 argv。监督器启动录像进程，发现完整事件后串行调度交付。 | [camera_demo](../../tools/camera_demo.cpp) · [resolve_camera](../../tools/resolve_camera.cpp) · [camera_service](../../tools/camera_service.cpp) |
| 主路采集与编码 | 1080p/15 FPS NV12。MPP 分配 DMA-BUF，V4L2 导入。DQBUF 后交编码器，匹配输入归还的 task、frame、PTS 和 buffer 才允许 QBUF。失败进入对应错误分支。 | [evrec](../../tools/evrec.c) · [编码输入](encoder-input-choice.md) |
| 副路采集与检测 | 主进程经 posix_spawn 启动同一二进制的私有 worker。640×360 NV12 经 RGA 转色、缩放及填边，写到独立的 640×640 RGB 模型输入。副路采集槽完成 RGA 读取后即归还；worker 独占 RKNN 上下文，完成推理及 INT8 输出连续扫描。 | [副进程入口与协议](../../tools/evrec_selfpath.h) · [模型输入](npu-input-choice.md) |
| 结果匹配 | Unix SOCK_SEQPACKET 非阻塞传递小消息。主路 poll 采集和结果 fd，核对代次、驱动帧号、时间戳、采集 epoch 和年龄。合法结果若比对应主帧先到，暂存一条等待配对。重复、逆序、错代、过期或源不匹配的结果均不能触发。 | [结果接收与归并](../../tools/evrec_selfpath.h) |
| 事件录像 | 主路维护连续命中、H.264 预录环与离场收尾。命中后从有 SPS/PPS 的 IDR 边界起写，有界写队列保存压缩包。独立交付程序封装 MP4、核对元数据并生成回执。 | [写队列](../../tools/evrec_writer.h) · [交付](../../tools/camera_delivery.cpp) |
| 网络分发 | 主路一份编码输出供录像与预览复用。每个客户端有自己的游标，锁内复制压缩数据，锁外发送，超时可释放慢端。 | [RTSP hub](../../tools/rtsp_hub.c) |

## 副路故障期间

主路保有编码器、预录和事件状态。副路失联时清除命中连续性，回收旧 worker 后重开；只有新一代与近期主帧相符的结果才能参与触发。检测长时间不可用会以 detection_unavailable 收尾当前文件，主路继续编码。共享传感器或 ISP 整体故障另走采集恢复流程。

## 与早期版本的关系

`detect` 保留串行基线；`DETECT_PIPELINE=1` 配合 `detect` 启用早期双 RGB 槽和检测线程。13.18→15 FPS 属于这两种模式的同负载对照。当前入口的副路进程、连续扫描和联合监听数据见[发布证据](../../evidence/2026-11-20-entrance-release/README.md)。
