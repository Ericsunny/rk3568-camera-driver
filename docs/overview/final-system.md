# 运行与配置

主入口camera_demo，监督器、录像器与交付工具分别为camera_service、evrec和camera_delivery。[架构](architecture.md)解释模块和所有权，[结果](../improvement/project-review.md)给出测试条件。

## 运行前的环境检查

完整构建依赖RK3568 Linux环境的MPP、RGA、RKNN头文件和运行库；Makefile的RGA_INC、RKNN_INC可覆盖，C++服务要求C++17。运行还需media-ctl、FFmpeg/FFprobe、pgrep、fuser、ss、sha256sum等工具。

```sh
make apps
```

检查sensor profile、媒体拓扑、NV12格式、节点占用、3A、端口、磁盘和模型哈希。采流期间保持rkaiq存活，不用独立工具同时写同一图像控制通路。程序不部署内核/设备树、不安装模型或自动切换现役服务。

## 标准配置

配置为[公共参数](../../config/final-demo.env)叠加sensor profile；模型路径和哈希须匹配实际环境，模型文件不随仓库提供。

| 项目 | 默认值 |
|---|---|
| 视频 | 1920×1080 NV12、15fps、GOP15、4000kbps |
| 事件 | pre3/post2秒、环64MiB、最长段30秒、冷却2秒 |
| 检测 | score0.45、连续3命中、每帧检测 |
| 网络 | profile端口、2客户端、发送期限5秒 |
| 空间 | 最小剩余1000MiB，无自动文件保留期 |
| 输入 | 启动器编码dmabuf，detect时NPU dmabuf |
| 架构 | DETECT_PIPELINE=0，默认串行 |

实际节点由resolve_camera按拓扑解析。裸evrec默认copy，复现时保存argv，不用另一入口的默认值推断运行状态。

## 启动与停止

```sh
./camera_demo imx415-isp manual /root/event-manual-run
# 预热后，在另一终端触发
 touch /root/event-manual-run/trigger
# 串行逐帧检测
./camera_demo imx415-isp detect /root/event-serial-run
# 并行逐帧检测
DETECT_PIPELINE=1 ./camera_demo imx415-isp detect /root/event-pipeline-run
# copy对照
ENCODER_INPUT=copy NPU_INPUT=copy ./camera_demo imx415-isp detect /root/event-copy-run
```

运行目录必须为新的绝对路径。流水线要求detect、NPU_INPUT=dmabuf、DETECT_EVERY=1；裸evrec拒绝触发文件并忽略SIGUSR1。标准C++入口支持该开关，历史shell入口不作为流水线入口。

前台Ctrl-C，或向所启动的入口PID发送SIGTERM并等待，使监督器停止recorder并处理已完成事件。共享板卡不要按名字批量kill。失败保留非零状态及未完成文件。

## 逐帧观测

```sh
FRAME_METRICS=/root/event-observation.csv DETECT_PIPELINE=1 \
    ./camera_demo imx415-isp detect /root/event-observed-run
python3 tools/analyze_frame_metrics.py /root/event-observation.csv
```

观测路径须为新绝对路径。输出frame CSV/.meta，流水线另有.detect.csv/.detect.meta。最多4096条，退出写盘，超量标记截断；默认关闭，不宣称零开销。

主机CI无需摄像头，使用编译器、合成数据与替身；不验证BSP、DMA一致性或SDK实际行为。输出目录保留配置、argv、事件H.264/JSON/包索引、MP4和交付JSON；仅有录像或recorder正常退出不等于交付complete。MP4不是精密逐帧时间线。

RTSP回环地址字段、长期文件与seen集合累积、物理响应及显示/准确率限制见[结果说明](../improvement/project-review.md)。发布源码不触发板端部署。
