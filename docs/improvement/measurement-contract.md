# 软件测量口径

media_ms/capture_ms是同一帧的媒体标签，用于排序、预录与封装；相减为零不是零延迟，阶段均值倒数不是实际fps。

| 指标 | 定义与限制 |
|---|---|
| DQ吞吐 | 每epoch去前30个完成帧，以首末DQ间隔与样本数计算 |
| 驱动缺口 | 同epoch的V4L2 sequence差；应用自增序号不能证明无丢帧 |
| 检测覆盖 | accepted/eligible，按全部帧；替换、过期、旧epoch、失败和截断另列 |
| DQ到应用 | 同机CLOCK_MONOTONIC；含预处理、排队、同步、推理/后处理及消费等待 |
| CPU成本 | recorder累计user+system/全部完成帧，含启动、收尾和观测输出；明确负载 |
| 时间分布 | 每epoch去前30帧，P95按0.95×(n−1)线性插值；损失/覆盖不去热身 |
| RSS | 就绪后至停止前；不等于全部DMA内存，有限平台不证明长期无泄漏 |
| 显示延迟 | 独立光学输入和显示观察，DQ/解码可用不代表屏幕发光 |

两CSV按epoch/frame_seq/driver_sequence关联。源DQ与frame观测DQ有少量软件间隔，不能当作同一次调用；驱动timestamp实际取样位置未独立核实，不当作精确曝光起点。

同工作量对照DETECT_EVERY=1；每两帧检测属于负载取舍。不同负载CPU百分比不能相减。4096项截断必须报告，不能继续声称完整分布。

[复算数据](../../evidence/2026-11-17-single-camera-release/README.md) · [结果](project-review.md)
