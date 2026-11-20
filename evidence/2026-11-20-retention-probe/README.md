# 短测复现

使用 RK3568 #62、发布提交 `585ba2dca16f84ba90a072552b1ba94a67d3ea16`。在新目录准备该提交的 Makefile、tools/camera_{service,delivery}.cpp、tools/camera_{process,json}.h、tools/evrec.c，以及本目录两个 Python 夹具。归档和夹具传板后分别核对 SHA-256。本轮保持现役 3A 和已有录像目录不动。

在含 Makefile 的源码目录运行（脚本路径按实际位置调整）：

```sh
make -j2 camera_service camera_delivery
python3 evidence/2026-11-20-retention-probe/probe.py "$PWD" > result.json
python3 evidence/2026-11-20-retention-probe/watermark.py tools/evrec.c > watermark.json
```

probe.py 只允许传入已编译程序所在位置，数据目录由 TemporaryDirectory 创建；每次从 FFmpeg 测试图生成视频。水位夹具直接编译源文件中的决策函数，替换时钟和空间查询。两次脚本退出码均为 0。result.json 的 samples 对每个会话保存各回执完成后的数组及监督器退出码。environment.txt 最后一行是结束时仍运行的 3A PID。

[保留策略与结论](../../docs/improvement/recording-retention.md) · [交付与回收观察](result.json) · [水位输入与输出](watermark.json) · [环境、产物与原始哈希](environment.txt)
