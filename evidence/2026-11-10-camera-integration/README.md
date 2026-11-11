# OV8858 ISP 接入与单路视频服务验证

2026-11-10，RK3568 厂商 Linux 4.19.232。记录对应本地未提交工作区和板端候选程序，**不能以基准 Git 提交代替实际文件指纹**；人员录像和画面仅留板端，本目录只包含日志、事件元数据和包时间记录。`SHA256SUMS` 是板端导出清单，已逐项核对；[内核构建输出](kernel-build-v4.log)另行从编译 VM 取回，SHA-256 为 `7dea32e759ab18696485143d1a0315ec4badde76474ddfc1aa89b34dbac5d451`。隔离内核树没有 Git 元数据，构建日志中有 `fatal: not a git repository`，不应隐去。

## 版本和条件

- 内核 #61 Image SHA-256 `b386f6d716538d3305efbc47d4b39da17697510647ca8d576c44910c7926c689`，DTB `eb38789325c307867f0d9d5b9b09c81123c48bc113cbd74af6d505877ccc6ce0`，OV8858 驱动源 `68cdd7d43766e9ca7cfd81f3216ebcbd688e479f26d930e35454ed688c510d54`。先串口临时引导，随后将其切为默认并正常重启，旧 #57 的 Image/DTB 留有恢复副本；[默认启动状态](default61/status.txt)。
- 板端 `evrec` 程序 SHA-256 `620f67a160df04d238e57cc8cd26e2bdf32328a611329f9848714a46b7b99509`。人体检测实验使用的交付脚本为 `09f0069ec3245af31cbc98d8c572f50ef3a29b8e315cba288aff6eb3ca0a788f`；后续交付失败修复及复测使用 `camera_service.py` `411f38d3b65b1e7c1d835645063ac91e45c40ee5ac314ffdcae92c9fc6a7a393` 与 `h264_to_mp4.py` `7e19e6f426cc3373aab2104911f2bb750d026fc678f8aabf9c27ad5d63101d41`。
- OV8858 IQ 来源于板上另一模组的参考文件，仅修改方向字段以匹配当前固定传感器方向；候选文件 SHA-256 `b86cca83604f91f061be8228756521510940bcda5e704ea8a1e15e33890fcd4f`。**这不是本模组的 IQ 标定或画质调优结果。**
- 两台相机分别运行，OV8858 ISP 输出 1280×960，IMX415 输出 1920×1080；不是双路同时检测。日志包含 librga 兼容模式提示；[3A 日志](rkaiq61.log)还记录了 `get drc params from 3a result failed` 等错误，其影响尚未单独定位，不能声称 3A 无错误或 IQ 已验证。该文件哈希 `af39bf75e6f52d606859132d0549d4f923cf1c87cf5c1b9486a8e2d7f317e842`；[#59 枚举失败日志](rkaiq59.log)哈希 `87ddcba0a7e5a8dc7f449d8aff78b7508e6acac03f74bdd11a5966b9381a84db`。

## 结果与原始记录

| 内容 | 观察结果 | 记录 |
|---|---|---|
| OV8858 人体事件 | 无人阶段无事件；入镜后触发，离开后收尾。两段 H.264 与 MP4 独立解码帧数分别为 451/451、412/412，服务退出 0。 | [检测日志与时间](ov8858-detect-diag-61b/service.log) · [解码计数](ov8858-detect-diag-61b/decode-counts.txt) · [参数](ov8858-detect-61b/argv.txt) · [分段交付记录](ov8858-detect-61b/event_20261110-153738-01.delivery.json) · [第二段](ov8858-detect-61b/event_20261110-153738-01-s1.delivery.json) |
| 双客户端 RTSP | 两种相机分别进行板内回环短测，两个客户端均完成 RTSP 协商和 RTP 收包，记录中无序号缺口。 | [OV 客户端 A](ov8858-rtsp-diag-61/client-a.log) · [B](ov8858-rtsp-diag-61/client-b.log) · [IMX 客户端 A](imx415-rtsp-diag-61/client-a.log) · [B](imx415-rtsp-diag-61/client-b.log) |
| 控制与画面变化 | #60 条件下更改曝光和增益后寄存器读回匹配，RAW 抽样均值发生变化；两项同时改变，不能分别归因，也不代表定量标定。 | [控制回读](ov8858-controls-60/readback.txt) · [RAW 抽样](ov8858-controls-60/raw-samples.txt) |
| 交付故障与修复 | 一次板端手动录像交付因 `ffprobe` 输出出现非 UTF-8 字节而返回失败；修正诊断文本解码后新一轮手动录像交付完成，157 帧，退出 0。 | [失败日志](ov8858-service-diag-61c/service.log) · [失败记录](ov8858-service-61c/event_20261110-154026-01.delivery.json) · [重测日志](ov8858-service-diag-61d/service.log) · [重测交付](ov8858-service-61d/event_20261110-154127-01.delivery.json) |
| 等待超时 | 一轮无人阶段等待超过 120 秒时限，退出码 124，未执行有人阶段；不计作完整实验。 | [日志](ov8858-detect-diag-61/service.log) · [退出码](ov8858-detect-diag-61/service.rc) |
| 默认启动重测 | #61 正常默认启动后，OV8858 与 IMX415 分别手动触发、封装完成，交付记录分别为 155 和 92 帧、服务均退出 0；启动记录及视频留本地。 | [版本](default61/status.txt) · [OV 日志](default61/ov8858/service.log) · [IMX 日志](default61/imx415/service.log) · [传输校验](default61/SHA256SUMS) |

默认重测内核增量日志记录了 [IMX415 `write reg array error`](default61/imx415/kernel-delta.log) 和两路测试中的 `rga2: unknown ioctl cmd!`；这次首帧与交付完成不证明这些日志没有影响，也不将其当成新修复。事件视频采用编码包时间跨度估算平均帧率封装，MP4 不保留逐帧采集时间；片段达到时长后按 IDR 切分，边界可能重叠。测试只覆盖现场及所列版本，不代表检测准确率、复杂网络或长时间稳定性。
