# 时钟记录

这是操作员根据本窗口终端输出整理的记录，不是系统自动校时日志。

- 校时前板端 `date -u +%Y-%m-%dT%H:%M:%SZ` 为 `2024-06-18T04:22:05Z`；主机 UTC 为 2026-11-05，板端 `timedatectl` 报告未同步、无 RTC。
- 主机取 UTC epoch `1793873500`，经 SSH 执行 `date -u -s '@1793873500'`，返回状态 0；板端紧接着读到 `2026-11-05T10:11:40Z`。主机执行前 UTC `2026-11-05T10:11:40.7910505Z`，执行后 UTC `2026-11-05T10:11:43.6194955Z`。
- `timedatectl` 仍报告 `System clock synchronized: no`、`NTP service: n/a`、`RTC time: n/a`。网络往返和执行耗时未经精确标定，因此不能宣称亚秒级同步；采集记录另保存 `/proc/uptime`。
- 未启停 3A 或其他服务，未更改 `/boot`。校时后的复查见 [基线日志](A1-baseline-readonly.log)。
