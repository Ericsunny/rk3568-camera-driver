# OV8858 自写驱动与厂商驱动同板短测

2026-11-14，在同一块 RK3568 板卡、同一个 OV8858 模组和 Linux 4.19.232 #62 Image 上对照。两次都使用传感器 1632×1224、同一 `evrec` 二进制、同一脚本和参数；静止场景由现场固定。测试计划见 [plan.md](plan.md)，执行脚本及各步骤返回码见 [self/result.json](self/result.json)、[vendor/result.json](vendor/result.json)。

厂商驱动已经编入 #62，测试时以[临时 DTB](dtb-diff.txt)更改 `compatible`，并为厂商驱动增加 `rockchip,ov8858-capture-mode = 1`。串口只做一次性引导，未改默认 Image、DTB、bootloader、分区或持久启动配置。Image、DTB、测试程序及参考 IQ 文件的 SHA-256 见 [frozen.sha256](frozen.sha256)。恢复时板端时钟因重启回退，已重新校时。[恢复后状态](restored-state.txt)确认重新绑定 `ov8858-sm`、3A 运行，默认 Image/DTB 哈希未变。恢复后未再进行完整采流回归。

## 观察结果

| 项目 | `ov8858-sm` | 厂商 `ov8858` | 边界 |
|---|---|---|---|
| 识别与模式 | 绑定成功，1632×1224 | 绑定成功，1632×1224 | [自写媒体图](self/media0.log) · [厂商媒体图](vendor/media0.log) · [厂商 probe](vendor-probe.log) |
| ISP 连续采集 | 90 帧，日志约 30.02 fps | 90 帧，日志约 29.88 fps | [自写日志](self/capture90.log) · [厂商日志](vendor/capture90.log)。两次短测不足以判断性能优劣或长期稳定。 |
| 输出帧 | 1 帧 NV12，2,996,352 B | 1 帧 NV12，2,996,352 B | [自写统计](self/frame-stats.json) · [厂商统计](vendor/frame-stats.json)。Y 均值分别为 122.959 / 116.646；自动曝光、实际 IQ 加载和其他状态未完全固定，不能把差异归因于驱动或判断画质优劣。原帧仅留板端，本轮未做人工画面检查。 |
| 控制项读取 | 曝光、增益、消隐、固定方向等可枚举 | 曝光、增益、消隐可枚举，本轮未看到方向控制项 | [自写控制项](self/controls_after.log) · [厂商控制项](vendor/controls_after.log)。本次未停止 3A 手动写控制，因此不证明各控制的设置效果。 |
| RTSP | 软件解码 30 帧，退出 0 | 软件解码 30 帧，退出 0 | [自写日志](self/rtsp_client.log) · [厂商日志](vendor/rtsp_client.log)。一个本机客户端，未测复杂网络或多客户端负载。 |
| 手动事件 | H.264/JSON 收尾，软件解码退出 0，探针计数 128 帧 | H.264/JSON 收尾，软件解码退出 0，探针计数 128 帧 | [自写记录](self/result.json) · [厂商记录](vendor/result.json)。本轮未测 NPU 自动触发或 MP4 交付。 |

每组包括连续采集、单帧采集和录像三次短时采流/停流；两组采集期间内核日志增量未匹配 `error/fail/warn/oops/timeout/reset/hung/stall/overrun`，这不等同于长期压力或排除所有异常。[自写增量](self/dmesg_delta.log) · [厂商增量](vendor/dmesg_delta.log)

## 差异与限制

- 厂商驱动启动时报告 `Failed to get power-gpios, maybe no use`，随后识别 sensor 并完成短测；其影响未单独验证。[probe 日志](vendor-probe.log)
- 厂商侧 3A 启动日志出现 `Enum sensor frame interval failed`，恢复默认自写驱动后的 3A 启动状态日志没有该行；这两份状态日志是在不同启动中取得，不能仅凭这条日志断言所有 3A 功能失效或良好。[厂商 3A](vendor-rkaiq-start.log) · [自写重启后 3A](self-rkaiq-reboot.log)
- 控制项存在具体差异：自写侧报告 HBLANK 2224、VBLANK 默认 20、曝光上限 1240；厂商侧分别为 616、26、2470。两者并非同一组接口参数。VM 中查看的厂商源码显示先以全分辨率初始化部分控制项、之后按 sensor ID 和设备树选择模式；这可能与报告差异有关，但尚未证明当前 Image 中对应源码的精确版本，也未通过修改模式验证根因。
- 设备树的 facing 值相同，但媒体实体名分别出现 `m01_b_ov8858-sm` 与 `m01_f_ov8858`。当前自写源码固定使用 `b`，VM 厂商源码根据设备树 facing 命名；两者对 3A 识别或图像的影响尚未验证。
- 参考 IQ 文件两次核对的哈希相同，但没有直接记录 3A 实际打开并加载的 IQ 路径；因此不能声称严格控制了实际 IQ。模组也未完成 IQ 标定。
- 厂商驱动的 3264×2448 模式、自写驱动的 1080p 模式、故障注入、长稳和跨版本兼容均不属于本轮。测试使用的工作树存在未提交改动，不能把 Git HEAD 直接当作完整源码指纹。

当前实测证明厂商驱动**在本板和本次配置下能够完成基础采集出帧、RTSP 软件解码和手动事件录像**，所以不能再把自写驱动描述成实现当前业务链路的必要条件。两者当前 3A 与控制项存在差异且实际 IQ 加载未确认；推荐保留已验证的自写驱动作为当前基线，厂商驱动作为可运行的对照方案，待帧间隔枚举与控制范围进一步验证后再决定是否切换。

原始画面与视频在板端 `/root/ov8858-driver-ab-20261114/`，未纳入仓库。脱敏日志打包后两端 SHA-256 均为 `6cf1ba906560d9a38b5da68b3a025852a1db599c6b4ee3c900ec69eb7ccfd13e`，见 [logs-only.tar.gz](logs-only.tar.gz)；单帧统计文件另行两端核对。[统计脚本](frame_stats.py)
