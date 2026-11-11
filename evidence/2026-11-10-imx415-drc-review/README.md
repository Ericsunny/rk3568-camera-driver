# IMX415 空寄存器表修复与 3A 参数对照

2026-11-10，RK3568、Linux 4.19.232。以文件哈希区分构建和实测，不将未提交工作区等同 Git 提交。录像含现场画面，只留板端；本目录包括日志、事件元数据和编码包时间记录。导出文件按 [SHA256SUMS](SHA256SUMS) 核对，完整串口记录留本地。

## IMX415 寄存器表

#61 所用 BSP 的 `supported_modes_2lane` 显式将 `global_reg_list` 置空，另有实际的模式寄存器表；原 `imx415_write_array()` 把空表打印为错误，却返回 0。启动路径将可选 global 表和必需模式表都送入该函数。原始 [#61 日志](../2026-11-10-camera-integration/default61/imx415/kernel-delta.log)可见该行，单凭它不能判断 I²C 传输已失败。

[补丁](../../driver/patches/imx415-optional-global-regs.patch)只在存在时写全局表；必需表缺失返回 `-EINVAL`，真实写寄存器错误继续向上传播。原 IMX415 源 SHA-256 `bc6eef3d1f4bd6bae7673feda5423ac796ce0d1959e15c0b22caa92f7162d54b`，打补丁后 `ece5cbb7032523fe633c883ce1b4925d15a0b856a99ba278ca0d266f7461ced6`；构建配置 SHA-256 `3f54fd62c30da1d8d47aa31cf46b38ee0114a64cbcbb2a2d31a276180104f068`。

使用已配置的 VM 内核树增量构建：首次因工具链 `libisl.so.15` 不在动态库搜索路径而[失败](kernel-build-imxfix.log)，加入该工具链的 `lib` 路径后[重试成功](kernel-build-imxfix-retry.log)。成功日志中的 `fatal: not a git repository` 来自隔离树缺少 Git 元数据；编译命令退出码为 0。主机替身从补丁后的真实 BSP 源抽取两个函数，验证可选 global、必需表缺失与模拟写寄存器失败的传播：[测试脚本](../../tools/tests/test_imx415_optional_global.py)。它不替代真实 I²C 测试。

候选 #62 Image SHA-256 `40ec35eccd8e78ecacaee883257f2446d056614c50f649ae2d4988806bb07927`，DTB 沿用 #61。先临时引导，再切换默认，正常重启后的[状态与恢复文件](final-status.txt)已核对。每台相机分别运行手动事件，IMX415 交付 90 帧、OV8858 交付 157 帧，均退出 0；[IMX415 录像日志](default62-imx415/service.log)与[内核增量](default62-imx415/kernel-delta.log)、[OV8858 录像日志](default62-ov8858/service.log)与[内核增量](default62-ov8858/kernel-delta.log)。IMX415 这次没有原空表错误；这不代表所有 I²C 故障或模式都验证过。增量仍有 `rga2: unknown ioctl cmd!`，用户态仍有 librga 兼容模式提示。

#62 默认启动后，两种相机分别进行了两个客户端、各约 5 秒的板内回环 RTSP 短测；两个客户端的协议请求成功、RTP 序号缺口为 0，服务退出 0。原始记录：[IMX415 A](rtsp-default62/imx415/client-a.log)、[B](rtsp-default62/imx415/client-b.log)；[OV8858 A](rtsp-default62/ov8858/client-a.log)、[B](rtsp-default62/ov8858/client-b.log)。这不等于对真实网络负载或长期运行的验证。

临时引导首次检查将 DTB 符号链接自身大小误当作目标文件大小，触发校验失败并复位回 #61；改用实际 DTB 路径后成功。两份串口原始日志留本地。

## DRC、merge 与参考 IQ

板端 `librkaiq.so` SHA-256 `1842c1b6096539d76b789b43d4fd7ab8f56677857a20b1f155fcf304b6a38839`，两台相机在流启动时均出现 `CamHwIsp21.cpp:364` 与 `:429` 的相同文字。具有相同函数名、文字及行号的 [Firefly RK356x 参考源码](https://gitlab.com/firefly-linux/external/camera_engine_rkaiq/-/blob/f7b676bcbd97dcdc6211ed498345eef5afc7259f/hwi/isp21/CamHwIsp21.cpp#L354)显示：364 行对应取 DRC 结果失败，429 行位于取 HDR merge 结果的分支，沿用了同一错误文字。参考源码文件 SHA-256 `0957f2445517ee1d799213e2e4bed4f6e4b55885433663d31254cbfe3435291c`。板端库导出同名函数，但尚未证明该参考源码与板端二进制是同一次构建，故这是有针对性的源码对照，不能当作精确源码匹配证明。尤其不能把两条日志都计为 DRC 故障。

一次受控短测保留现用参考 IQ 作[基线](iq-ab/baseline/rkaiq.log)，另一臂使用板上 2022 年另一模组的 OV8858 参考 IQ，仅将方向字段 `CISFlip` 从 3 改为 1；两臂均按 [预设条件](iq-ab/criteria.txt)采集 40 帧到 `/dev/null`、退出 0，均出现 364/429 两条启动日志。后一臂额外出现 `can't find 1632x1224 in lscResName`，因此未采用；[对照日志](iq-ab/alternate/rkaiq.log)。采流结束后已恢复原 IQ 并启动 3A，[恢复哈希与进程](iq-ab/restore-status.txt)。这不能证明 DRC 成功或失败的最终根因，也不能对两份非本模组校准数据作画质判断。

两臂采集命令相同：

```sh
timeout -s TERM -k 5 25 v4l2-ctl -d /dev/video14 \
  --set-fmt-video=width=1280,height=960,pixelformat=NV12 \
  --stream-mmap=4 --stream-count=40 --stream-to=/dev/null
```

其中设备编号是本次媒体拓扑下的实际值，复现时需重新核对。两次采用的 IQ 均不是本模组标定文件；未保存测试画面。

此前资料将原因确定为 IQ schema 不匹配；现有对照不足以证明这一归因，暂视为待验证假设。要真正修复 DRC 并确认效果，需要精确匹配现役 `librkaiq` 的源码/构建及本模组 IQ，测量实际下发参数，再做固定场景的画面 A/B；不可通过隐藏日志或随意编造标定数据冒充修复。
