# 单 IMX415 入口留证发布验收

2026-11-20，在 iTOP RK3568、厂商 Linux 4.19.232 #62、运行中的 rkaiq 上，取暂存树中的 Makefile、配置和应用源文件，在新的板端目录原生编译。GCC/G++ 9.4.0；应用使用板上已有 MPP、RGA、RKNN 头库及配置模型。源码归档两端核对 SHA-256 后解包，使用 tar -m 避免板钟偏差干扰全量构建。

## 重现本轮

在目标机的仓库根目录操作，选择两个全新、可写且满足磁盘水位的绝对目录。夹具需要 python3、FFmpeg、空闲的 TCP 18596 和可读内核日志。保留厂商 3A，先看本机设备占用。测试在运行目录旁建立独立日志，前台等待服务正常退出。

```sh
make -j2 entrance
sha256sum evrec camera_demo
python3 evidence/2026-11-20-entrance-release/smoke.py "$PWD" /var/tmp/entrance-check-001 entrance
python3 evidence/2026-11-20-entrance-release/smoke.py "$PWD" /var/tmp/manual-check-001 manual
```

| 入口 | 主路完成/采集 | 驱动序号缺口 | 板内 RTSP 严格解码 | 结果与交付 |
|---|---:|---:|---:|---|
| entrance | 96/96 | 0 | 90 帧 | 93 个唯一结果，1 份 96 帧 MP4 回执 complete |
| manual | 156/156 | 0 | 90 帧 | 通过触发文件交付 1 份 89 帧 MP4 |

两次服务退出码均为 0，完整新增内核日志未命中夹具检查的 CRC、Oops、panic 特征。自动模式产生了事件；本轮没有逐帧标注现场人员，不能用于识别准确率。另一次通过新编译的 entrance 入口，独立 x86 Linux VM 经局域网 RTSP/TCP 用 FFmpeg 严格解码 60 帧，报告 dup=0、drop=0。其他播放器和长期运行需单独验证。[入口结果](entrance.json) · [手动结果](manual.json) · [逐帧记录](entrance/main.csv) · [结果接受记录](entrance/side.csv) · [实际参数](entrance/argv.txt)

本轮 evrec SHA-256 为 `339755fdacd79b7c6a6140258869b549266395eb92a7dd9abe00a7912416f945`，与此前最终副路故障验收相同。新入口 camera_demo 为 `863d0f4a6a018201288d32cfdbae92574ebec06ffe59978f1d27f7c678116c1e`。测试使用新二进制、新目录及专用端口，没有安装启动服务或更换内核。

## 同一工程的不同测量

| 实验与对照 | 可核对的结论 |
|---|---|
| 早期串行 / 双 RGB 槽；另一次关闭检测的编码输入对照 | 同逐帧工作量的采集 13.18→15.00 FPS；关闭检测时共享输入省一次 NV12 复制，录像进程 CPU/帧降低 15.9%。[早期原始数据与复算](../2026-11-17-single-camera-release/README.md) |
| 在真实副路比较两种 INT8 扫描顺序 | 保持模型、阈值、逐帧检测与观看负载，ABBA 各轮稳态分别计算主副进程 user+system CPU 时间，按主路源帧数归一，再对同组两轮取平均。22.65→15.09 ms/帧，降低 33.4%。[各轮结果](../2026-11-19-selfpath-scan-ab/summary.json) |
| 两组均开启连续扫描，比较帧尾读取与联合监听 | 驱动单调时间戳到主路接受的 P95 从 104.81 降至 76.32 ms；结果就绪后等待的 P95 从 37.27 降至 0.39 ms；两组稳态均为 542/542。各轮先算 P95，再取同组平均。[各轮结果](../2026-11-19-selfpath-poll-ab/summary.json) |
| 入镜、有效阳性被接受与文件打开分开计时 | 光学可见起点受采样与遮挡影响。人员补测没有证实文件打开也提前约 28 ms。[逐轮时间边界](../2026-11-19-person-open/summary.json) |
| 副路中断、过期/错代阳性回放、重开 | 主路 363/363、约 15 FPS、零序号缺口；回放 57 条阳性，接受 0、新开事件 0，新鲜阳性随后仍可触发。两段交付分别 96、87 帧。[故障与文件核对](../2026-11-19-final-selfpath-fault/run-01/summary.json) |
| IMX415 能力枚举候选 #64 | 27/27 个针对性查询判据及 1080p/15 开流、重开通过，恢复默认 #62。枚举小尺寸支持的帧率不证明对应小尺寸已实流。[对照与回退](../2026-11-18-imx415-interval/summary.json) · [独立内核补丁](../../driver/patches/imx415-frame-interval-compat.patch) |

各历史 JSON 是原测量的摘要；本次运行保留新轮次的逐帧数据。实机画面和模型不随源码分发。当前仍需独立验证另一块同型板从干净 BSP 部署、存储保留策略及长时间无人值守运行。
