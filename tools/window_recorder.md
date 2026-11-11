# 板端窗口记录器

`window_recorder.py` 启动独立子进程，在持久目录按单调时钟采样 `/proc`、空间与温度，向该子进程周期发送 SIGUSR1，并保存服务日志和 `dmesg --follow-new` 内核日志。结束时仅向其启动的子进程发 SIGTERM，不发送 SIGKILL；D 状态或不能及时退出需要人工处置。输出目录唯一创建，保留失败和未完成记录，不自动删除文件。程序退出 0 只表示定时记录、采样和子进程收尾条件满足，**不是 24h 验收 PASS**。

运行前按现场验收方案单独确认窗口授权、现役服务和 3A、设备身份、测试命令、温度阈值、空间与 inode 水位及容量计算。Python 3、能使用 `dmesg --follow-new --nopager` 的权限、对应 `/proc` 读取权限和持久文件系统是前置条件；实际设备依赖未验证。预算必须覆盖所有事件视频、日志及其他输出，余量与 `--reserve-bytes` 分开计算；传入的数字是执行前冻结的配置，不是自动推算。禁止把 `/tmp`、tmpfs 或 `/boot` 当存储目录。子进程的所有输出路径必须由操作者检查，程序无法验证任意命令行不会写到目录外。

示意（绝对路径、设备、数值与配置均需按现场填写；先短测）：

```sh
nohup python3 /path/to/window_recorder.py \
  --parent /persistent/camera-window --duration 86400 --interval 60 \
  --first-trigger 120 --trigger-every 90 \
  --budget-bytes CALCULATED_BYTES --reserve-bytes RESERVED_BYTES --reserve-inodes RESERVED_INODES \
  --temperature /sys/class/thermal/thermal_zoneX/temp --max-temp PREAGREED_MILLIC \
  -- /isolated/evrec -d /dev/IDENTIFIED_NODE [其他冻结参数] -o '{run}/events/ev' \
  > /persistent/camera-window/launcher.log 2>&1 < /dev/null &
```

`{run}` 会被替换为实际唯一路径；启动后记录器打印该路径。使用独立会话或 `nohup` 并实测断开 SSH 后继续运行。示例中的占位值不能直接执行。必须检查 evrec 接受 SIGUSR1 触发及正常 SIGTERM 收尾，触发间隔足以让上一次事件结束，日志级别和输出前缀不触及现役数据；不能停止或杀死 3A。

产物有启动参数与可执行文件哈希 `manifest.json`，`service.log`、新增 `kernel.log`、`samples.csv`、实际发送信号的 `triggers.jsonl` 和 `summary.json`。每行采样含 PID+启动 tick、boot_id、RSS/PSS（无权限时 PSS 留空）、线程/fd、磁盘和温度；采样与触发使用 monotonic，墙钟仅供关联。到达时长、采样无缺口且子进程退出 0 才返回 0；温度/空间阈值、D 状态、可识别 Oops/panic、进程身份变化或内核记录器退出会中止。失败的持久化使运行无效，磁盘耗尽可能导致无法写出 summary，应保留现场。

限制：运行前必须另存内核日志基线；`dmesg --follow-new` 仅采集跟随启动后的记录，内核日志丢失或权限变化需要另核验；关键词检查不是完整 error/Oops 分析。`triggers.jsonl` 只证明信号已发送，不能证明事件生成、关闭或可解码。服务输出和内核输出由各自进程写文件，记录器不能保证它们所有写入错误被报告；验收时须检查日志完整性、实际文件、事件与解码、采集/编码计数、掉线和预注册资源阈值。不要把此工具单独输出作为验收通过。断电、缺样、重启、设备失联或缺少关键指标时，按现场方案判为无效或未定。

无板测试：在 Linux 上从非 `/tmp` 工作目录运行 `python3 tools/tests/test_window_recorder.py`。该测试使用模拟服务、温度和内核日志进程，不验证摄像头、真机持久存储、温度传感器或 24h 连续性。
