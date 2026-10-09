# mini3a — 用户态 3A 闭环(教学实现)

RK3568 + IMX415 上**不依赖 rkaiq** 的 3A 控制环:采集线程(V4L2 完整循环)与
3A 主线程通过零拷贝的 buffer 所有权环形队列协作;AE 用 `VIDIOC_S_EXT_CTRLS`
直接驱动传感器曝光/增益,AWB 走灰世界假设由 `isp_wb.ko`(sysfs→ISP 寄存器)
执行数字增益。

- `mini3a.c` 带六个标注手术点(`TODO(你)`),为**驱动岗学习主线**设计:
  先读官方 [capture-example](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/capture-example.html)
  再填 `frame_stats / ae_step / awb_step / 收敛状态机`。
- 配套任务书与寄存器手册见项目 kb(`13-mini3a任务书.md`)。
- `isp_wb.c`:`sysfs → 0xfdff0138/13c(AWB 数字增益)+ 使能位` 的内核执行器,
  也是"内核模块替用户态写寄存器"的最小示例。

上游背景:rkaiq v6.0x6.1 与 4.19.232 内核 vendor UAPI 存在代差
(stats buffer 4 字节错位已修,初始 params 死锁留档缓修),本实现绕过之。
