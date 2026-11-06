# evrec QBUF / RGA 故障注入测试构建

本夹具只链接到隔离的测试副本，不替换现役 evrec。它用 GNU ld `--wrap` 重定向被链接目标文件中的直接调用，不是全局 ioctl 拦截器，也不拦截共享库内部调用。`evrec.c` 和 `rtsp_hub.c` 当前的 ioctl 调用均有第三参数；增加目标文件或调用后必须重新审计，不能用于其他程序。`imresize_t` 的声明必须与实际目标 RGA 头和库一致。

示意命令（以实际目标的头、库位置及项目构建命令核对后使用）：

```sh
gcc -O2 -pthread -DHAVE_RGA -DHAVE_RKNN \
  -I/usr/include/rga -I/root/npu/3rdparty/rknpu2/include \
  tools/evrec.c tools/rtsp_hub.c tools/evrec_device_fault_wrap.c \
  -Wl,--wrap=ioctl -Wl,--wrap=imresize_t \
  -lrockchip_mpp -lrga -lrknnrt -lm -o /isolated/test/evrec-fault
```

上述命令是待目标环境核对的构建模板，不是已完成的板端链接证据。先检查链接器输出、符号和二进制哈希。使用原程序参数运行独立副本，并将 stderr 保存在独立测试目录：

```sh
EVDF_MODE=qbuf EVDF_HIT=1 EVDF_CAMERA=/dev/videoN /isolated/test/evrec-fault ...
EVDF_MODE=rga EVDF_HIT=1 /isolated/test/evrec-fault ...
```

`EVDF_HIT` 是从 1 开始的命中序号；只能指定一次故障。QBUF 模式必须指定实际识别出的绝对路径字符设备：仅该设备成功 DQBUF 后的下一次 QBUF 可被计数，初始入队不注入。命中返回 `-1/EIO`，不调用真实 QBUF；应观察真实程序进入恢复路径。RGA 模式命中直接 `imresize_t` 调用并返回 `IM_STATUS_FAILED`，不调用真实 RGA；应独立观察该帧未调用 NPU、后续成功帧恢复。夹具不提供推理调用计数，缺少可审查的观测手段时该判据仍未验证。

`[evdf] HIT` 日志给出进程、操作、序号、设备及模拟返回；未见命中日志的实验无效。配置无效或无法写入命中日志时以 125 退出。命中记录本身不证明业务行为正确；还须核对退出码、事件状态及设备运行。不要将此夹具用于并行打开同一设备的多个独立采集线程，它只跟踪单个待重排队 fd。

`python3 tools/tests/test_evrec_device_fault.py` 在 Linux 上用替身测试实际 evrec 分支、首个/第二个命中、一次性注入、其他设备及 GNU ld 重定向；VM 上另用厂商 RGA 头分别完成 x86 和 AArch64 对象编译。这些均不是完整目标程序链接或板上运行。按 [实验计划 A1](../docs/41-板测窗口实验设计.md) 取得窗口授权，保持 3A 纪律，记录命中与结果后才能判定板测。
