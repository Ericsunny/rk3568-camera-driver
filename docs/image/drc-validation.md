# OV8858 DRC 参数与效果验证

OV8858 通过 CIF/SDITF 接 ISP，正常业务由 rkaiq 负责图像控制。原参考 IQ 中 DRC 的开关为 0；之前把启动日志当作“DRC 无法下发”，证据不足。#62 板端观察到：仅将该开关设为 1 后，3A 的 ISP 参数 QBUF 出现 DRC 启用与配置位，驱动状态接口报告 `HDRDRC ON`；启动日志仍会出现，不能单凭该日志判断后续状态。[参数记录与实验条件](../../evidence/2026-11-10-drc-investigation/README.md)

为区分开关和调节效果，固定场景、手动曝光与增益后进行了两组关—开—关式对照：参考 IQ 的 DrcGain=1 时，开关 DRC 前后全帧亮度统计接近；DRC 保持开启，临时将 DrcGain 从 1 改为 2 后，平均 Y 明显上升，再改回 1 后接近基线。AWB 增益有少量漂移，图像只保存在板端；结果支持本场景中参数对图像有影响，不代表画质更好或已经完成本模组标定。

目前板上 OV8858 的参考 IQ 默认启用 DRC，保留参考 DrcGain=1；实验值 2 没有设为默认。原 IQ 留有备份。IMX415 IQ 未随此修改。两种传感器分别运行，重启或复现前应核对板端内核、IQ 和服务实际状态。

可用 [IQ 候选生成器](../../tools/create_drc_iq_variant.py)基于现有参考文件生成新文件；它拒绝覆盖原文件，默认只启用 DRC。`--gain` 是明确的实验选项，不产生标定数据：

```sh
python3 tools/create_drc_iq_variant.py reference.json drc-enabled.json
# 如需单独做实验，可指定 --gain 2；不能将此值当作通用画质调优结果
```

该命令只生成文件，不会安装或切换运行中的 IQ。实际切换须在采流结束后处理 3A、备份并校验文件；不在采流中停止 3A。参数观察器和局限见[工具说明](../../tools/isp_params_observer.md)。
