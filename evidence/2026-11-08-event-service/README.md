# 事件视频服务运行基线

日期：2026-11-08。默认内核已切换为 `4.19.232 #57`，正常启动后 rkaiq 恢复自启。固定应用目录为 `/root/camera-service-final`。本记录以[源码清单](source-inputs.sha256)和[程序/环境指纹](baseline.json)识别测试版本；输入来自基准提交 `6d03563` 之后的工作树，当次尚未形成新提交。不能把基准提交本身当成全部被测源码。

## 最终范围与实测

主服务为 IMX415 → ISP/rkaiq → NV12 → 可选人体检测 → MPP H.264 → 事件文件/RTSP，关闭的事件分段自动封装为 MP4 并生成交付记录。OV8858 保留驱动与 RAW 采集支路，camd 为独立队列验证工具；mini3a 本轮验证 AE 和 params 通路，AWB 图像效果未得到证明。

| 项目 | 实际结果与证据 |
|---|---|
| 默认启动与环境 | [默认启动记录](acceptance57/default/baseline.txt)：#57，3A 实际进程存活且自启启用，命令行无临时 3A mask。Image SHA-256 为 `e94a08bf9dd701370a293dfffa75fbc70fb1af139c0211219060bceed5de2245`；DTB、模型、库与程序指纹见 baseline.json。 |
| 最终应用完整编译 | 板端 GCC，RGA+RKNN 双宏，`-O2 -Wall -pthread`，退出 0、无警告；[编译输出](build/evrec-model-v4.log)。日志为空表示无编译器诊断，程序指纹为 `5c7a6b3de61f779d48439b16d0c8624f2fcae973fa18cefb8a3efeef3bd64abc`。 |
| 手动事件与自动封装 | #57 上一次事件[原始 JSON](acceptance57/manual/event_20261108-211317-01.json)、[交付 JSON](acceptance57/manual/event_20261108-211317-01.delivery.json)及[完整日志](acceptance57/manual.log)：H.264/MP4 均解码 100 帧，交付 complete，服务退出 0。此后修改的模型处理不改变手动模式写盘路径。 |
| 最终应用人体事件 | [阶段标记](acceptance57/final-model/stages.txt)、[完整输出](acceptance57/final-model/detect.log)、[退出码](acceptance57/final-model/detect.rc)。无人阶段无事件；入镜后连续命中、自动开启，离开后结束。本次一个事件分两段：[段0](acceptance57/final-model/detect/event_20261108-213208-01.delivery.json)为408帧、max_duration；[段1](acceptance57/final-model/detect/event_20261108-213208-01-s1.delivery.json)为208帧、target_left。两段均完成源/容器解码帧数核对，服务退出0。段边界有一帧重叠，不能把二者之和当作不重复源帧数。 |
| 最终应用双客户端 | [客户端A](rtsp-clients/rtsp-final-v4-a.log)、[客户端B](rtsp-clients/rtsp-final-v4-b.log)：并发、协商均200、收到RTP、序号缺口为0，两个检查均 PASS。只验证本次短时场景。 |
| 慢客户端隔离 | [停止读取的客户端](rtsp-clients/slow-client.log)、[正常客户端](rtsp-clients/rtsp-fast-with-slow.log)、[服务端日志](acceptance56/manual-v2.log)：慢端出现 `why=stall errno=11` 后断开，快端继续接收且 gaps=0。测试应用早于最终模型修正，RTSP 模块字节未变，见[模块对照](module-equivalence.json)。 |
| 网络码流解码 | [解码退出码](acceptance56/rtsp-decode.exit)、[stderr](acceptance56/rtsp-decode.stderr)：FFmpeg 接收 TCP RTSP 流并以 `-xerror` 解码，退出0、错误输出为空。该项在 #56/前一应用上执行，不冒充 #57 最终应用的重复执行结果。 |
| 写盘故障 | [板端结果](acceptance56/file-errors/results.json)：控制组退出0；JSON打开、视频短写、JSON关闭三类故障各命中一次，退出1，不发布成功JSON。被测 writer 的4个函数与最终源码相同，见模块对照；最终源码还通过[主机故障测试](host/model-v4.log)。 |
| OV8858 | #56 的[PM与90帧采集](acceptance56/ov8858/pm-and-capture.txt)：空闲0/suspended、采流1/active、停后0/suspended；与主视频同时运行。两帧RAW共5,013,504字节、数据不同。#57 的[30帧重测](acceptance57/ov8858/capture30.log)正常。驱动源指纹相同。 |
| camd | #57 [结果](acceptance57/camd/result.json)：60帧采集、60帧归还、退出0，无QBUF错误。只验证默认双消费者模式，未把所有实验选项视为已验收。 |
| mini3a | #56 [trace](acceptance56/mini3a/trace.log)定位params长度不足，随后以最小BSP修复构建#57。#57 [程序输出](acceptance57/mini3a-output.log)在80帧达到原定亮度目标、退出0，保存3,110,400字节NV12。但[像素检查](acceptance57/mini3a-pixels.json)显示UV恒128，不能据AWB LOCKED文本宣称白平衡效果通过。 |

人员素材和原始像素文件仅保留本地；本目录不包含人像视频或图片。视频哈希和处理记录可用于现场/本地副本复核，不能声称可从本仓库直接下载本次人员素材重算。

## 本轮修复的实际问题

- 事件原始 JSON 的跨度漏算预录包，已补入首末包时间；JSON 改为写 `.pending.json`，写入和关闭成功后才发布 `.json`。封装器只处理最终JSON，保留原始文件，另写 `.delivery.json`。
- 板端 ffprobe 依赖的库会把初始化信息写到 stdout，破坏 JSON 输出。改读明确的 compact 流记录，其余输出保留在日志；解码器 stderr 报错仍视为失败。未把库告警从记录中删掉。
- RTSP 检查工具读取 PLAY 响应时可能吞掉后续 RTP 字节。最初[客户端A失败](rtsp-clients/rtsp-two-a.log)保留；socketpair 复现并修正读取边界后，两客户端重测通过，没有通过忽略序号缺口来改变判据。
- #57 修复 ISP v21 分配函数使用 v20 结构大小的问题。[补丁](../../driver/patches/isp21-params-buffer-size.patch)只有一行；[内核构建](kernel57/build.log)退出0，但有隔离树缺少Git元数据的诊断，未称为无诊断构建。原 #52/#55/#56 备份保留，未改 bootloader、分区或持久服务文件。
- 最初一次“无人”录像实际有人员在画面边缘，作为无效负样本保留在 `acceptance56/detect*`；#57 后续受控场景还出现了误触发，见 `acceptance57/default/controlled*`，未抹去这些结果。随后按[Rockchip YOLOv8说明](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov8/README.md)、[输入路径](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov8/cpp/rknpu2/yolov8.cc)和[后处理](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/yolov8/cpp/postprocess.cc)修正模型调用：letterbox+114补边、UINT8输入交由runtime转换、类别概率作为置信度、同类NMS、逐项输出索引与长度校验。阈值仍为0.45/连续3帧。最后一轮场景通过；本轮未单变量归因，不将全部改善归给其中一项，也不据单场景宣称检测准确率。
- NPU运行时采样到D状态，后续[调用栈](acceptance56/detect-state-sample.txt)位于rknpu同步提交等待，程序持续出帧并正常退出。本次没有持续挂死；不据此推翻旧W6事故。

## 检查与结束状态

[最终主机日志](host/model-v4.log)包含实际函数的几何/填充/NMS/输出索引测试（ASan/UBSan）、收尾错误与preload故障测试、预录时间、启动检查、真实FFmpeg合成码流集成和socketpair边界测试。[mini3a主机日志](host/mini3a-diagnostics.log)覆盖完整程序替身的12类情形；这些均不作为板端图像效果证据。本轮未执行远端CI。

新日志经哈希核对与敏感模式检查，串口原始登录输出未入库。[结束状态](final-state.txt)确认板端正常运行#57和单个rkaiq守护进程，测试采集进程已结束，视频节点空闲。后续按此有限功能基线运行与维护；MP4为平均帧率近似时间轴，mini3a的AWB效果仍属于独立实验边界。
