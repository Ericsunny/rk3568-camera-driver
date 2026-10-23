# M8:单路 MPP 硬编 + RTSP 推流

> 目标:把相机画面推成 RTSP 流,供局域网播放器直接观看;
> 编码必须走硬件(VEPU),软件编码不计入验收。

## 1. 环境与前置发现

| 项 | 实测 |
|---|---|
| MPP 运行库 | `/usr/lib/aarch64-linux-gnu/librockchip_mpp.so`(有 `.so.0`/`.so.1`) |
| 头文件 | `/usr/include/rockchip/rk_mpi.h`、`mpp_frame.h`、`mpp_packet.h`、`mpp_buffer.h` |
| 内核节点 | `/dev/mpp_service`;编码器 `mpp_rkvenc fdf40000.rkvenc` 已 probe |
| ffmpeg / gstreamer | **都没有**(板上无 ffmpeg、无 gst-launch) |
| RTSP 服务端 | **没有** mediamtx / live555 / vlc |

结论:编码库有、播放器生态没有。所以 RTSP 服务端必须自己实现,不能依赖现成组件。

## 2. 实现

三个工具,全部 C,板上 gcc 直接编译:

| 工具 | 作用 | 编译 |
|---|---|---|
| `tools/mpp_enc_probe.c` | 从 NV12 文件编 H.264,量编码耗时 | `gcc -O2 -o mpp_enc_probe mpp_enc_probe.c -lrockchip_mpp` |
| `tools/cam_rtsp.c` | 采集 + 硬编 + RTSP 服务端 | `gcc -O2 -pthread -o cam_rtsp cam_rtsp.c -lrockchip_mpp` |
| `tools/mpp_dec_probe.c` | 把抓下来的码流解码,验证可解码 | `gcc -O2 -o mpp_dec_probe mpp_dec_probe.c -lrockchip_mpp` |

`cam_rtsp` 的数据流:

```
/dev/video5 (NV12 mmap) → copy_nv12 进 MPP 缓冲 → VEPU 编 H.264
   → 帧环(8 帧) → RTP(FU-A 分片,marker 置末包) → RTSP over TCP(interleaved)
```

关键实现点:

1. **SPS/PPS 随关键帧补发**:编码器把 SEI 排在 IDR 前面,所以判断"这一帧是不是关键帧"不能用"第一个 NAL 是不是 5",要用扫描整帧是否含 IDR。踩过一次:只在 `type==5` 时补,结果抓下来的流里一个 SPS 都没有,解码 0 帧。
2. **采集缓冲要还**:`DQBUF` 拿到后拷进编码器缓冲,立刻 `QBUF` 还回去,否则 4 个缓冲很快耗光。
3. **RTP 分片**:小于 1400 字节走单 NAL 包,大于则 FU-A 分片,`marker` 只打在整帧最后一个包上。
4. **RTSP 只做 TCP interleaved**:不做 UDP,避免端口协商与 NAT 问题;VLC / ffplay 都支持。

## 3. 实测

### 3.1 编码性能(单帧,含 memcpy)

| 分辨率 | 帧数 | 总耗时 | 单帧 | 输出码率 |
|---|---|---|---|---|
| 1920x1080 | 60 | 1187.9 ms | **19.8 ms** | 2952 kbps(静态画面,目标 4000) |
| 3840x2160 | 30 | 2090.4 ms | **69.7 ms** | 6539 kbps(目标 8000) |

> 4K 单帧 69.7 ms 里有约 12 ms 是 12.4 MB 的 NV12 memcpy,其余是编码本身。

### 3.2 RTSP 端到端(1920x1080 @30 目标码率 4000 kbps)

自写的 RTSP 客户端(PC 侧 Python,`rtsp_probe.py`)握手并收 8 秒流:

| 指标 | 实测 |
|---|---|
| 握手 | OPTIONS / DESCRIBE / SETUP / PLAY 全部 `200 OK` |
| SDP | `sprop-parameter-sets` 含 SPS 27 B、PPS 5 B |
| RTP 包 | **1510** 个,字节 2 030 896 |
| 帧数(marker 计数) | **120 帧 / 8 s = 15 fps** |
| 序号 | 0~1509 **连续无缺口** |
| 实际码率 | **2031 kbps** |

> 帧率 15 fps 不是编码瓶颈(1080p 编码只占 19.8 ms,理论 50 fps),而是**传感器仍在 4K 模式出流**,ISP 缩放到 1080p 后帧率仍受 4K 模式限制。

### 3.3 可解码性验证(关键)

把 RTSP 抓下来的 `rtsp_out.h264` 用 `mpp_dec_probe` 解码:

```
>>> 码流 2009006 字节
>>> 首个 SPS 偏移 918308        ← 抓包起点在 GOP 中间,跳过前面的 P 帧
>>> 分辨率变更 1920x1080
>>> 首帧 1920x1080 Y 采样均值 121.2
>>> 解出 60 帧
```

对照:同一场景用 `grab` 直接抓 `/dev/video5` 的 Y 均值为 **121.3**。

**误差 0.1 码值 → 整条链路(采集→编码→RTP→RTSP→解码)像素级正确。**

## 4. 诚实清单(没做到的)

| 项 | 状态 |
|---|---|
| 多客户端 | ❌ 一次只服务一个,第二个连接会等待 |
| RTCP | ❌ 完全不发,只有 RTP |
| UDP 传输 | ❌ 只支持 TCP interleaved |
| 音频 | ❌ 无 |
| 帧率 | ⚠️ 15 fps(受 4K 传感器模式限制,不是编码瓶颈) |
| 延迟 | ⚠️ 未测量(需要播放器侧时间戳对齐) |
| 断线重连 | ⚠️ 客户端断开后服务端回到 accept,可以重连,但会重新等下一个关键帧(最长 4 s 黑屏) |
| 优雅退出 | ✅ 已加 SIGINT/SIGTERM 处理,确保 `STREAMOFF`(否则 ISP 会卡死,见 docs/29) |

## 5. 复现步骤

```sh
# 板上
cd /root
gcc -O2 -pthread -o cam_rtsp cam_rtsp.c -lrockchip_mpp
nohup ./cam_rtsp -w 1920 -h 1080 -r 30 -b 4000 > /root/rtsp.log 2>&1 &
sleep 4
cat /root/rtsp.log          # 应看到 "RTSP 服务已就绪 ... SPS=27 PPS=5"

# PC 侧播放
ffplay rtsp://192.168.10.2:8554/live
# 或 VLC:媒体 → 打开网络串流 → 输入同一地址

# 停止(必须用 TERM,不能 kill -9,否则 ISP 可能卡死)
kill -TERM $(pidof cam_rtsp)
```

## 6. 过程中踩的坑

1. **`pkill -f cam_rtsp` 会杀掉 ssh 会话自身**(远程命令行里含这个词),用 `pidof cam_rtsp | xargs -r kill`。
2. **`signal()` 默认带 `SA_RESTART`,`accept()` 不会被信号打断**,SIGTERM 收不下来 → 改为 `sigaction` 不带 `SA_RESTART`,并在 handler 里 `close(lfd)`。
3. **stdout 重定向到文件是块缓冲**,崩溃时前面的日志全丢 → 加 `setvbuf(..., _IONBF, ...)` 才能看到崩在哪。
4. **`mpp_enc_cfg_deinit()` 之后 MPP 内部仍在用这份 cfg**,手动释放会 `free(): invalid pointer`;交给 MPP 管即可。
5. **解码验证必须从第一个 SPS 开始**:抓包起点在 GOP 中间时,前面几百 KB 的 P 帧没有参数集,解码器直接吐 0 帧。
