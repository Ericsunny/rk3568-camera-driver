# 双路检测进程状态连续观察

2026-11-10 在 #62 内核上对两路检测进程进行约 22 秒的有限观察；本目录于 11 月 11 日从板端导出。实验预设、采样代码、原始结果及退出状态分别见[计划](dual-progress-watch-20261110/plan.json)、[采样脚本](dual-progress-watch.py)、[逐次采样](dual-progress-watch-20261110/samples.jsonl)、[观察结束状态](dual-progress-watch-20261110/watch-status.json)和[双路服务状态](dual-progress-watch-20261110/run/status.json)。测试不包含受控的人体进出、RTSP 客户端或检测准确率评估，画面和录像均留板端。

按约 0.25 秒间隔记录进程状态、等待位置和 ISP 输出计数。两路都多次观察到 `D` 状态且等待位置为 `rknpu_submit_ioctl`，也观察到其他状态；采样过程中 ISP 输出帧计数持续增长，两条日志均有推理统计。结束后双路服务返回 0，3A 为 active，设备无占用。该现象支持本次采样期间设备调用存在等待且系统仍有进展；**采样无法判定每次等待的精确持续时间，也不能排除其他时段发生卡顿**。本轮不是双路检测、事件录像和推流的联合验收。

日志包含 librga 兼容提示，完整保留在[IMX 日志](dual-progress-watch-20261110/run/imx415.log)与 [OV 日志](dual-progress-watch-20261110/run/ov8858.log)；[内核增量](dual-progress-watch-20261110/dmesg-delta.txt)随记录保存。打包文件 [progress-raw.tar.gz](progress-raw.tar.gz) 的板端和本地 SHA-256 均为 `6b72b4f925b911e2c55f229da428836bf9da3e8e8e56adb8e2d8e410fce1019e`。
