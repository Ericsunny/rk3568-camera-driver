# 编码输入的共享与归还

标准camera_demo默认ENCODER_INPUT=dmabuf；裸evrec默认copy。共享模式由MPP分配NV12 DMA-BUF，V4L2导入，减少采集到编码的一次整帧CPU复制。CPU叠字需要缓存同步，ISP/RGA/编码硬件访存及编码包复制仍存在。

copy完成复制后可较早QBUF；共享模式必须在MPP归还输入并核对task、frame、PTS与buffer后才QBUF，收到编码输出包不等于输入归还。目标BSP不支持或共享失败时显式报错，不隐式换成copy；由操作者设置ENCODER_INPUT=copy作为兼容和对照。

2026-11-17关闭检测和观测，各4轮/504帧的加权recorder CPU成本5.8351→4.9084ms/完成帧，约下降15.9%，两者均约15fps。观测开启的另组DQ→QBUF中位约1.911→13.643ms，体现共享输入延后归还的代价。数据口径不同，不将阶段分布与CPU百分比相减。

历史OV8858恢复测试出现CSI CRC并停止；copy也有同类记录，根因未闭合，不能归因于共享或记为已解决。当前IMX415单路结果不覆盖双路/恢复组合。

[数据与条件](../improvement/project-review.md) · [缓冲生命周期](architecture.md) · [运行配置](final-system.md)
