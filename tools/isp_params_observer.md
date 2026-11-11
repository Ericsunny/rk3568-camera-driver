# RKISP21 参数观察工具

`isp_params_observer.c` 是本项目 #62 板端实验用的 LD_PRELOAD 观察器。它记录参数设备的 MMAP 和 `VIDIOC_QBUF`：只复制参数结构开头的三个 64 位标志及帧号，并记录 ioctl 返回值；不记录图像、不修改参数。

工具使用本次设备节点 `/dev/video13` 和 `/dev/video22`，不是自动解析的通用设备发现工具。运行前应通过 `media-ctl -p` 核对它们分别为 ISP 的 `rkisp-input-params`，确认使用 RKISP21 参数结构。只在隔离测试中预载到 3A 进程；该工具对所有参数 ioctl 做跟踪，会增加开销，不作为生产服务依赖。更换内核、库、节点或内存模式后必须重新验证。

```sh
gcc -shared -fPIC -O2 -Wall -Wextra -Werror -o isp_params_observer.so \
  tools/isp_params_observer.c -ldl -pthread
python3 tools/analyze_isp_params_trace.py qbuf.log
```

3A 的隔离切换必须先确认采集结束，备份并核验现役库与 IQ；测试结束后恢复系统服务。不在采流中停止 3A。观察器只支持 MMAP 的 metadata-output 参数缓冲；`mapped=0`、QBUF 失败、映射表溢出或没有记录时，分析器返回非零，结果不可用。追踪本身可能影响调度；QBUF 成功不等于已经在对应帧生效。

本次实验遇到的旧用户态 ioctl 请求在 64 位上被符号扩展，比较时仅取低 32 位；实际调用继续透传原值。初版失败及修正过程、参数记录和原始测试范围见 [DRC 研究记录](../evidence/2026-11-10-drc-investigation/README.md)。`drc_frame_metrics.py` 是该次 1280×960、4 帧 NV12 的固定尺寸统计脚本，不负责判断拍摄条件或画质好坏。
