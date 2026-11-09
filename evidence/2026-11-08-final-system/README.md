# 最终系统候选：构建与板端准备

后续已形成[事件服务运行基线](../2026-11-08-event-service/README.md)；本页保留#56及早期应用的准备快照，不作为当前部署状态。

检查日期为主机记录的 2026-11-08；板钟仍为 2024-06-18，**尚未校准**。本目录先记录准备结果，不表示最终版本已经验收、部署或冻结。最终功能范围见[系统设计](../../docs/overview/final-system.md)。

## 已执行

- 板端原生 GCC 9.4.0，使用板端 RGA/RKNN/MPP 头文件与库，完整构建 `evrec`、`camd`；mini3a 使用从 VM BSP 提取的 UAPI 构建。三个目标退出均为 0、编译输出无警告，见 [board-build-v2.log](board-build-v2.log)。运行时源码来自 Git 暂存树 `9861afb4fc43cbce2d97e1cb463648280bbbacf2`，相关源码及程序 SHA-256 在 [candidate-preflight.json](candidate-preflight.json) 中；该 JSON 是这次准备的快照，不会随之后的验收改写。
- 编译 VM 的独立内核树增量构建当前 OV8858 源码，完整 `Image` 构建退出 0，见 [输入指纹](kernel-inputs.txt)与[编译器输出](kernel-build.log)。输出为 `4.19.232 #56 SMP Sun Nov 8 11:39:44 UTC 2026`，Image SHA-256：`53f8f3aeab51a6ef1e7b029e46516fcbced49639c2000f9fbd78751710e58b36`。日志有一次 `fatal: not a git repository` 的版本元数据查询诊断，不能称为无诊断构建；构建仍继续完成。基础为已有厂商 BSP 工作树，不声称它与板端 #52 除本驱动外完全相同。
- 新 Image 从 VM 经主机复制到板端独立目录，两次传输均校验 SHA-256；未覆盖 `/boot`。默认 #52 Image SHA-256 为 `46a4dab391a9980de22a628ceae4f8d7838fd5215e1b7e1e3f7cd1845e48a5b7`，板端与 VM 备份均重新核对一致。旧 #55 候选镜像及备份也核对一致。
- 实际板端为 #52，只有一个 `/usr/bin/rkaiq_3A_server` 进程，检查时两个采集节点未被占用、未发现 D 状态任务。systemd 单元显示 active 不等于有有效 MainPID；启动器另外检查实际守护进程。
- 仅在 #52 上进行 `timeout 15s v4l2-ctl -d /dev/video5 --stream-mmap=4 --stream-count=30` 基线采集，退出 0，输出 30 帧标记、15.00 fps，见 [capture](baseline52-capture.log)。未将其用作 #56 或新应用的验收结果。
- 该采集的 [dmesg 增量](baseline52-dmesg.txt)保留了 `imx415 ... write reg array error`。候选 BSP 的 IMX415 写表函数在传入空寄存器表时打印该文本后返回初始化为 0 的 ret；这解释了可出图的一个机制，不能把所有同类 I²C 错误都归为无害，更不能称本次 dmesg 干净。没有改厂商 IMX415 代码去隐藏诊断。
- Linux VM 执行固定入口替身测试、evrec 源码块错误传播测试和 preload 集成测试，见 [host-tests.log](host-tests.log)。源码块测试中的未用变量警告属于抽取后的测试程序，与板端完整目标的零警告不是同一口径；RGA-only 抽取块通过也不表示完整 RGA-only 应用受支持。主机测试不访问真实设备。

## 尚未执行

新内核引导/默认部署；当前候选应用的事件录像、双客户端、慢客户端、正负场景人体触发；当前驱动的 RAW/双路采集；无其他 3A 控制器时的 mini3a；camd 当前候选的真实设备运行；本轮板端写盘故障传播。

启动器最后调整为不在启动时预建 trigger，避免把既有文件首次 mtime 当成手动操作。最新 Linux 替身测试见 [launcher-check-v3.log](launcher-check-v3.log)，其脚本/配置/测试 SHA-256 同时记录；不覆盖前一版日志。板端独立目录也已核对同一启动器与配置。

下一步只验收系统设计中列出的有限功能，不把历史性能数字、所有命令行组合或长期可靠性实验作为该版本已完成的证明。
