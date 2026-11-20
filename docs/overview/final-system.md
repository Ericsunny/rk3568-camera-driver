# 构建与运行

## 目标环境

- 已验板为 iTOP RK3568、IMX415、Linux 4.19.232 #62、GCC/G++ 9.4，C++ 应用要求 C++17。运行前核对板钟、媒体图与已运行的厂商 rkaiq。此入口使用现有板卡镜像。
- 准备 MPP、RGA、RKNN 的目标头文件和匹配的动态库。Makefile 支持 RGA_INC、RKNN_INC、CC、CXX 等覆盖项。目标板还需要 media-ctl、FFmpeg/ffprobe、pgrep、fuser、ss、sha256sum。用户需能读写视频节点、访问硬件加速设备及输出目录。
- 自行提供 [配置](../../config/final-demo.env) 对应的 YOLOv8n RKNN 文件。默认位置为 `/root/npu/examples/yolov8/model/yolov8n_rk3568_i8.rknn`，SHA-256 为 `722d3aca582872b1b9a22ccc7cb3fee48e4c97fe725fcd138655db5ff7d1210d`。移动同一模型时修改配置中的 MODEL，保留哈希校验。更换模型需要重新验证输入、输出及检测结果。
- 交付入口解析厂商拓扑名称，预期进程为 /usr/bin/rkaiq_3A_server。换 BSP 要重新核对媒体图、IQ、驱动、用户态库。#64 帧间隔枚举候选补丁独立于应用，使用 #62 即可运行本配置。

## 启动、检查与停止

```sh
make -j2 entrance
./resolve_camera imx415-isp
./resolve_camera imx415-isp --selfpath
./camera_demo imx415-isp entrance /var/tmp/entrance-run-001
```

运行目录必须尚不存在，默认磁盘预留 1000 MiB。新机可先确认两个节点确实连到同一传感器、两路均无其他采集业务。应用保存 argv.txt、inputs.sha256 和内核标识。入口固定逐帧检测、编码与检测输入共享、连续扫描和联合监听，冲突的实验开关会报错。默认录像 1080p/15 FPS、GOP 15、4000 kbps、64 MiB 预录环；人员阈值 0.45，连续三次命中，pre/post 分别 3/2 秒，实际可回溯时长随关键帧变化。

前台用 Ctrl-C，后台向该次 camera_demo 启动的 PID 发送 SIGTERM 并等待。监督器等待录像器退出并处理已完成事件。检查 *.delivery.json 的 status、帧数和关联文件。MP4 采用平均帧率近似。上述单次运行目录完整保留产物。

## 不依赖人员入镜的交付检查

```sh
./camera_demo imx415-isp manual /var/tmp/manual-run-001
# 预热后，在另一终端执行
touch /var/tmp/manual-run-001/trigger
```

局域网观看地址为 `rtsp://板卡地址:8554/live`。需要避免端口冲突时使用 RTSP_PORT_OVERRIDE。已用独立 Linux VM 上的 FFmpeg 经局域网严格解码。当前 RTSP 响应包含回环绝对地址，其他客户端的 URL 解析兼容性需实测。默认最多两个客户端，发送期限 5 秒。

## 启用录像循环保留

在录像卷上创建服务账号独占的专用目录，以下以已挂载的 /srv 为例。每次服务启动都可以使用同一个 next 参数，程序会分配新的运行序号。录像容量和剩余空间触发条件见[保留策略、保护标记与验证](../improvement/recording-retention.md)。

```sh
install -d -m 0700 /srv/entrance-recordings
CAMERA_RETENTION_ROOT=/srv/entrance-recordings CAMERA_MAX_MIB=1024 CAMERA_TARGET_MIB=768 \
  ./camera_demo imx415-isp entrance /srv/entrance-recordings/next
```

## 对照与观测

- `camera_demo imx415-isp detect` 加新目录运行历史串行配置；显式设置 DETECT_PIPELINE=1 使用早期双缓冲线程。历史 Shell 脚本和裸 evrec 有各自默认值。推荐部署使用上面的 C++ entrance 入口。
- 设置 FRAME_METRICS 为新的绝对文件路径，可记录主路 CSV/.meta。entrance 另有 .side.csv（源时间、接受时刻、代次及接受/拒绝原因）。主路观测表最多 4096 条，副路文本记录会随运行增长。持续运行时按需要开启观测。
- 已验证的命令、二进制指纹和本轮验收范围见[发布证据](../../evidence/2026-11-20-entrance-release/README.md)。
