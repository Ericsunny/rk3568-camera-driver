# 提交快照编译与回归

2026-11-14，在 RK3568 Linux 4.19.232 #62 上，对本次暂存的源码快照做独立编译。板端 `rkaiq_3A` 为 active，未启动采集程序，也未覆盖现役二进制。

- 暂存快照中 `tools/evrec.c` SHA-256：`71718ae633483d0a62932313b7eb1d2eb4faba778a7c30b80c9d2ce01e196ef2`。
- 暂存快照中 `tools/rtsp_hub.c` SHA-256：`4f5d05afc019268baf7ebac56629997d4c4192348d3440073e70256748a49408`。
- 传输归档两端 SHA-256：`c3656a5df505777107146d3ca08f2a9be3868ea99f7d7a60afd8102ad762d379`。
- 板端独立目录：`/tmp/camera-dmabuf-commit-dp5Dc0`；目录为临时文件，重启后不保证保留。

```sh
gcc -O2 -Wall -pthread -DHAVE_RGA -DHAVE_RKNN \
  -I/usr/include/rga -I/root/npu/3rdparty/rknpu2/include \
  -o evrec tools/evrec.c tools/rtsp_hub.c \
  -lrockchip_mpp -lrga -lrknnrt -lm
```

编译退出码为 0，编译器未输出警告；生成的二进制 SHA-256 为 `3a84b524e0b9175d8ae4abbfeb15b9889f0bfb6a2117e52747e73284d4401802`。本次仅编译，**没有运行该二进制**；之前的板测使用另一独立候选二进制，其 RTSP 源码包含另行准备的修改，不能当作此提交快照的完整板测。

相同暂存快照在 Linux VM 上运行共享输入归还、启动参数、录像故障注入、预录与封装等主机测试，以及相关驱动控制、RTSP 和双路服务测试，均通过。主机替身不能代替设备和长期运行验证。
