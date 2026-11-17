# 检测输入的共享与边界

标准camera_demo在detect模式默认NPU_INPUT=dmabuf；裸evrec默认copy。编码输入ENCODER_INPUT独立选择。

共享路径由RKNN分配RGB内存，RGA通过fd写入目标区域，配合缓存同步及rknn_set_io_mem绑定输入。当前固定模型为640×640、NHWC INT8、1输入/9输出，应用提交UINT8、pass_through=0，运行库保留转换。去掉选定应用层复制不代表runtime无复制、native直通或整个系统零拷贝。

默认串行在采集主循环中运行RKNN。DETECT_PIPELINE=1增加第二个RGB槽，由单检测线程交替绑定并独占context，主线程处理采集、编码和事件；只接受detect、dmabuf、every=1。历史camd/zerocopy实验的native结果不转移到本服务。

[架构与所有权](architecture.md) · [结果与限制](../improvement/project-review.md) · [运行配置](final-system.md)
