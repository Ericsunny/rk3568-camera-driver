# IMX415 手动事件录像与 MP4 封装

2026-11-06 在 RK3568 板端使用 IMX415 采集、手动触发一次事件，将 H.264 码流离线封装成 MP4。画面为静态场景。本次验证录像和播放文件，不涉及目标检测或 RTSP 客户端。

## 文件

- [event.h264](event.h264)：板端输出的 H.264 裸码流，SHA-256 `6236de96cd379662667bce6c5c079802817e7a30dd7820212c5e67f0ae87b191`。
- [event.json](event.json)：程序输出的事件元数据，SHA-256 `d96ebc0f157da118e77cf9c3a767fc563deb958471c68ac70724bdb4b1d0fed9`。
- [event.mp4](event.mp4)：使用 FFmpeg stream copy 封装的播放文件，SHA-256 `2fba34c87bca40a8023d4f8786a9cd7865064cbe84322d42890dad31a7d3b65e`。
- [record.log](record.log)：板端程序输出；[board-decode.log](board-decode.log)：板端解码时的 stderr，包括 librga 版本探测与兼容模式提示；[first-attempt.txt](first-attempt.txt)：首次启动包装脚本的失败说明。

## 版本和环境

- 录像时本地仓库 HEAD `7b6936cef23587cc0d008e2ae2fd5324630d1713`；`evrec.c` SHA-256 为 `eb3349ef1b952275fb5b7abc4dd385f3294b6f5cd3631338f77214a54f069661`，与 [准备方案](../../docs/acceptance/recording-demo-plan.md) 中的应用源码基线一致。
- 板端内核 `4.19.232 #52`；`rkaiq_3A_server` 运行中。板端驱动镜像与当前仓库驱动源码没有做一一对应，不能把本次录像视为当前 OV8858 驱动板测。
- 在独立临时目录传输 `evrec.c`、`rtsp_hub.c`、`rtsp_hub.h`，板端与本地 SHA-256 一致；使用板端 gcc 按准备方案双宏编译和链接，退出码 0、编译日志为空。独立二进制 SHA-256 `031b97122c77b9957c1fd9cdb3c705b7c40503e1448d6a92511e746206b1da42`，不覆盖现役程序。
- `/dev/video5` 在录像前查询为 3840×2160 NV12，bytesperline 3840，sizeimage 12,441,600 字节；未停止 3A 或部署驱动。板端时钟与主机查询相差约 2 秒，未校钟。

## 执行与核验

隔离目录内运行：

```bash
RUN_DIR=/root/evrec-demo-BcLnYv
timeout -s TERM 40 /tmp/evrec-demo-9QrmcZ/evrec-demo -d /dev/video5 -w 3840 -h 2160 \
  --pre 1 --post 2 --ring-mb 64 --kbps 4000 --max-events 1 --recover 0 \
  --trigger "$RUN_DIR/trigger" -o "$RUN_DIR/event"
```

启动约 8 秒后创建 trigger 文件；程序正常退出，退出码 0。日志记录触发源帧 `seq=112`、预录起点 `pre_seq=88`，事件以 `target_left` 收尾；JSON 记录 `write_error=false`，文件长度与其 `bytes=1012733` 一致。H.264 与 JSON 从板端复制后，主机与板端 SHA-256 一致。

板端 `ffmpeg -v error -i event.h264 -f null -` 退出码 0；stderr 有上述 librga 兼容模式提示，不能描述成 stderr 为空。Windows 主机 FFmpeg 7.1 解码 MP4 退出码 0；FFprobe 统计 H.264 和 MP4 均为 56 个解码帧。帧数一致和解码退出码 0 不能单独证明时间精度或所有录像场景均正确。

MP4 使用 `python tools/h264_to_mp4.py event.h264 event.mp4 --fps 15` 封装，15 fps 是播放近似值，非逐帧实测时间戳。裸码流探测报告 30 fps；FFmpeg 封装提示输入包没有时间戳。输出 MP4 的 3.7 秒来自封装时间基准，不作为原始事件时长的精确测量。当前 JSON 的 `first_ts_ms` / `last_ts_ms` 在这次记录中对应触发后流式写入区间，不包含前面已从环中写出的预录部分；因此 `duration_ms=2005.4` 也不是整段文件时长。

第一次启动包装脚本误用了目录模板，日志重定向失败，录像进程未启动；修正目录后按同一参数运行并生成上述结果。
