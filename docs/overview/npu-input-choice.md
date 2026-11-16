# 检测输入的 DMA-BUF 共享

检测分支支持两种输入方式：标准启动器在检测模式默认选择 `dmabuf`，将 RGA 输出写入与 RKNN 绑定的输入缓冲，去掉应用层逐帧 RGB 补边移动，使用绑定输入替代逐帧 `rknn_inputs_set`。直接运行 `evrec` 时仍默认 `copy`。编码输入的 `--encoder-input` 独立选择。

目前的实现只接受已核对的 640×640 NHWC INT8 模型输入契约。RGA 写入 UINT8 RGB，RKNN 绑定时保留 runtime 输入转换（`pass_through=0`）；共享一个输入缓冲不代表 runtime 内部没有中间缓冲或 DDR 访问。ISP 写帧、RGA 读写、编码器或 NPU 的硬件访存也仍然存在，不应称为整个系统无拷贝。

满足板端构建、模型与设备配置后，可在检测模式启用：

```sh
ENCODER_INPUT=dmabuf NPU_INPUT=dmabuf \
  ./camera_demo imx415-isp detect /absolute/path/to/new-run
```

启动器可显式指定 `ENCODER_INPUT=copy NPU_INPUT=copy` 选择复制路径；手动模式不启用 NPU 共享输入。`NPU_INPUT=dmabuf` 要求检测模式和支持该参数的程序；模型契约不符时拒绝启动。本地曾进行两路同帧对照、短时手动录像与 RTSP 测试，原始记录未收录，不能在仓库内独立复核。没有测量 DDR 带宽或证明 runtime 内部没有复制，长期稳定性与其他模型兼容性仍待验证。

检测仍在采集主循环中同步执行。默认每帧检测；可用 `DETECT_EVERY=2` 启动器参数或 `--detect-every 2` 让每两帧检测一次。连续命中次数按检测帧计算，采样会增加触发延迟并可能漏掉短暂出现的目标；该选项是性能与检测时效的取舍，不是并行流水线，默认保持 1。
