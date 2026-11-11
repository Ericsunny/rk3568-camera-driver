# 双路并发短测：2026-11-10

这次测试使用未提交工作区，基准 HEAD 为 `534a6b7da10aaadcebc2d2160f87020165d14f10`，不能用 HEAD 代替实际文件指纹。板端 Linux 4.19.232 #62、现役 rkaiq，IMX415 经 `/dev/video5` 输出 1920×1080 NV12，OV8858 经 `/dev/video14` 输出 1280×960 NV12；节点来自当时的媒体拓扑。内核及 IQ 指纹见[采集输入](dual-v4l2-smoke-20261110/inputs.sha256)。板端 `evrec` SHA-256 为 `620f67a160df04d238e57cc8cd26e2bdf32328a611329f9848714a46b7b99509`，本地 `tools/evrec.c` 为 `d3db5470fb3ec762ebc4f84a257a804d55a90742ea80c12a115ac867f73ec448`；测试程序采用既有板端二进制，启动脚本是本地修改后的版本。

[采集测试脚本](dual-v4l2-smoke.sh)和[两路编码启动脚本](dual-encode-start.sh)为测试前传到板端并核对 SHA-256 的副本。双路检测使用 `run_dual_camera.py`，SHA-256 `fb6f9d6acb7c0891b4b415f5934b33afdfdecab479193bf82939ebd17d99d0fc`。

| 实验 | 结果与限制 | 记录 |
|---|---|---|
| V4L2 同时采集 | 两进程重叠，各输出 180 帧至 `/dev/null`，退出码均为 0；日志分别显示约 15.00 和 30.02 fps。ISP 快照中 IMX 输入侧 `frameloss:3`，输出侧为 0；未判断该计数来源，也未验证持续零丢帧。 | [并发进程](dual-v4l2-smoke-20261110/processes.txt) · [IMX 日志](dual-v4l2-smoke-20261110/imx415.log) · [OV 日志](dual-v4l2-smoke-20261110/ov8858.log) · [ISP 快照](dual-v4l2-smoke-20261110/isp0.txt) · [退出码](dual-v4l2-smoke-20261110/status.txt) |
| 两路编码、RTSP 与手动事件 | 两个独立 `evrec` 分别监听 8554、8555；重试时两个回环客户端并发软件解码，分别得到 150 和 300 帧、退出码均为 0。两路手动事件分别完成 MP4 交付，随后用软件解码器解码 MP4，退出码为 0、错误输出为空。此为有限回环短测，不代表广域网或长稳。 | [并发进程](dual-encode-20261110/retry-overlap.txt) · [IMX 解码](dual-encode-20261110/imx-retry.log) · [OV 解码](dual-encode-20261110/ov-retry.log) · [IMX 交付](dual-encode-20261110/imx/event_20261110-210530-01.delivery.json) · [OV 交付](dual-encode-20261110/ov/event_20261110-210530-01.delivery.json) · [MP4 解码命令与结果](dual-encode-20261110/mp4-decode-verification.txt) |
| 双路检测观察 | 两个进程各有连续检测和 RKNN 耗时日志，记录的阶段耗时折算值约 8.6 fps/路；两路均生成由 SIGTERM 收尾的事件文件。未控制入镜场景，不能据此断言识别正确。现场进程快照观察到两路 `D` 状态后提前停止；随后两个子服务退出码均为 0、设备释放、3A 保持运行。单次快照无法判断其持续时间或证明卡死，也不能证明该模式稳定，双路 RTSP + 检测及受控人体事件未验收。 | [IMX 日志](dual-detect-20261110/imx415.log) · [OV 日志](dual-detect-20261110/ov8858.log) · [退出状态](dual-detect-20261110/status.json) · [现场观察](detect-observation.md) |

首次 RTSP 检查使用板端 FFmpeg 默认的 `h264_rkmpp` 解码器，两个客户端退出码为 0 但输出均为 0 帧，**该次判为失败**；重试前另记判据并指定 `-c:v h264`、明确帧数，结果见表。两者的不同不足以单独确定首次零帧的根因。[首次 IMX](dual-encode-20261110/imx-client.log) · [首次 OV](dual-encode-20261110/ov-client.log) · [重试条件](dual-encode-20261110/retry-criteria.txt)

手动事件发生在首次客户端尝试期间；成功解码的重试发生在事件收尾之后。因此这轮证明了同一次双路运行内分别完成手动事件和双路解码，**尚未证明事件写盘期间双路客户端同时成功解码**。

采集测试后的 3A 为 active；检测停止后再次确认 3A active、两个节点空闲。各阶段内核增量见 [采集](dual-v4l2-smoke-20261110/dmesg-delta.txt)、[编码](dual-encode-20261110/dmesg-delta.txt)、[检测](dual-detect-dmesg-delta.txt)；日志里的 librga 兼容提示及 MP4 封装时间戳提示未被当作消失。MP4 使用平均帧率近似，未保留逐帧采集时间。本目录不含现场图像或录像，录像留板端；源日志打包文件 [raw-logs.tar.gz](raw-logs.tar.gz) 双端 SHA-256 均为 `6a95b900c32e927531beb1efc5da222d951b565c9e2407948de5c25e0cfadf0c`。
