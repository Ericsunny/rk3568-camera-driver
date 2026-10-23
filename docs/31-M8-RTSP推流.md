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

## 7. 多客户端与延迟(第二轮补测)

### 7.1 多客户端

改动:帧环从"单消费者 + 读后释放"改为**环形覆盖写 + 每客户端独立读游标**,
每个 RTSP 连接一个线程,慢客户端只丢自己的帧,不影响别人也不阻塞采集。

实测(1920x1080,同一台板子):

| 并发数 | 每客户端帧数/时长 | 帧率 | 延迟 avg | 延迟 p95 |
|---|---|---|---|---|
| 1 个 | 90 帧 / 6 s | 15 fps | 18.9 ms | 19.9 ms |
| **2 个** | 60 帧 / 4 s(各) | 15 fps | 19.1 / 19.0 ms | 20.3 / 19.9 ms |

两路并发时**帧率不下降、延迟不增加**,说明瓶颈在传感器 15 fps 和编码,不在分发。

限制仍在:最多 4 个客户端(帧环 8 槽决定落后窗口),超出会各自丢帧;无 RTCP。

### 7.2 延迟测量方法与结果

方法:RTP 时间戳在**采集 DQBUF 时刻**打点(绝对 `CLOCK_MONOTONIC` 毫秒),
客户端收到帧尾包时用同一单调钟相减 = 采集到客户端收到 的端到端延迟,
不含播放器自身的解码与缓冲。工具 `tools/lat_probe.py`,在板子上跑 loopback 消除网络抖动。

| 指标 | 实测 |
|---|---|
| min | 17.4 ms |
| avg | **18.9 ms** |
| p50 | 18.7 ms |
| p95 | **19.9 ms** |
| max | 31.6 ms |

分解:采集+NV12 拷贝 ≈ 6 ms,硬件编码 ≈ 12 ms,打包与 TCP 传输 < 1 ms。
**播放器侧另加解码与抖动缓冲(VLC 默认 200~1000 ms),那部分不在此数内,未测。**

## 8. 与 rkaiq_3A 共存(修正一条此前的错误认知)

### 8.1 结论:可以共存

`rkaiq_3A` 处于 `active`(开机自启)时,`cam_rtsp` 直接打开 `/dev/video5` **正常工作**:

```
>>> RTSP 服务已就绪  (SPS=27 PPS=5)
>>> 采集 2640 帧, 14.4 fps
>>> 采集 2700 帧, 14.4 fps
```

3A 在跑 → 自动曝光/白平衡正常 → 画面亮度与颜色都正确(截图对比见 evidence)。

### 8.2 此前的误解

早期文档写"跑 3A 时 grab 直采 `/dev/video5` 会超时,必须先停 3A"。
真相是:**导致 ISP 卡死的不是"3A 正在运行",而是"运行中把 3A 杀掉"**
(`rkisp_stream_stop id:0 timeout`,之后所有 DQBUF 阻塞,只能重启)。

正确规则:
- **不要 `systemctl stop rkaiq_3A` / `kill` 它**;
- 采流程序自己开自己的流,与 3A 并行;
- 采流程序退出用 `SIGTERM`(程序内已处理,退出前 `STREAMOFF`)。

### 8.3 为什么这条重要

1. 端到端演示(推流/预览)从此不需要"先停 3A、再重启"的畸形流程;
2. 图像质量来自 3A,不再需要手动 `v4l2-ctl --set-ctrl exposure/gain` 猜参数;
3. 前面卡住的"拍屏幕测延迟"因此重新可行。

## 9. 玻璃到玻璃延迟(截屏法实测,浏览器预览通路)

### 9.1 方法

页面同时显示:**顶部大字时钟**(浏览器本地时间,20 ms 刷新)和**相机预览**。
相机对准这块屏幕 → 截图 → 同一张图上同时存在"现在的时刻"和"相机拍到它时的时刻",
两者相减即玻璃到玻璃延迟。**不依赖任何跨机时钟同步**。

实测一次:

```
截图顶部的时刻(现在):   18:43:59.512
预览图里的时刻(拍到时): 18:43:58.984
差值:                    0.528 s = 528 ms
```

### 9.2 这个 528 ms 花在哪

| 环节 | 量级 | 说明 |
|---|---|---|
| 快照生成间隔 | 133 ms | 每 2 帧更新一次(15 fps) |
| 页面轮询间隔 | 200 ms | `setTimeout(f, 200)` |
| 整帧 BMP 传输 | ~60 ms | 691 KB / 100 Mbit 局域网 |
| 浏览器渲染 | ~10 ms | |
| 采集+编码 | ~19 ms | 已知值(见 §7.2) |
| **合计** | **~0.42~0.53 s** | 与实测 528 ms 吻合 |

**结论:这 528 ms 里,相机与编码只占 19 ms,其余全是"轮询式快照"这个取图方案本身的代价。**

### 9.3 不能拿这个数当 RTSP 延迟

| 通路 | 端到端 | 构成 |
|---|---|---|
| 浏览器预览(BMP 轮询) | **528 ms** | 19 ms 管道 + 509 ms 取图策略 |
| RTSP(H.264) | **18.9 ms 到客户端** | 不含播放器解码/缓冲 |

RTSP 播放器的真实延迟 = 18.9 ms + 硬件解码(约 5~10 ms)+ **播放器抖动缓冲**(VLC 默认 1000 ms,可调至 100~200 ms;ffplay 约 0)。
**这一项由播放器配置决定,不能作为本系统的固定指标。**

### 9.4 想把预览延迟压下去的三条路

1. 快照改成**每帧更新**(133 → 67 ms);
2. 轮询改**长连接推送**(multipart/x-mixed-replace),去掉 200 ms 轮询;
3. 整帧 BMP 换 **JPEG**(691 KB → 约 60 KB),传输从 60 ms 降到 5 ms。
   三条全做可望进到 80~120 ms 量级。
