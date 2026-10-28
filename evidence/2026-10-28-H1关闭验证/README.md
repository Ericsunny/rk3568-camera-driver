# 2026-10-28 H1 关闭验证(commit=830bf53)

## 绑定(防伪三元组)

- commit = 830bf53(用户手改 hub:706 断串后推送的修复提交)
- 源码 md5 = 082835fa33(tools/rtsp_hub.c, PC 与板端一致)
- binary_md5 = 2fb0376b1e(板上 /tmp/evrec_h1, gcc 9.3.0, 2024-06-18 16:40 CST)

## 过程注记

首两次 DoD 尝试失败原因 = `/root/ev` 目录被此前清理命令删除 → 日志重定向失败 →
服务进程从未启动(与代码无关)。mkdir 后启动成功。

## DoD 结果(全过)

| DoD | 结果 |
|---|---|
| 1. ffprobe 跨网段 | `codec_name=h264, width=1920, height=1080` ✓ |
| 2. PC ffmpeg 5s 拉流 | 服务端 `sent=120→211, drop=0`(两次会话累计) ✓ |
| 3. SDP sprop | `RTSP/1.0 200 OK` + `sprop-parameter-sets=Z0QQM6wbGqB4AiXlhAAAAwAEAAADAPI8IhGo,aM4xsBs=` ✓ |

服务端存活, fd=14 无增长。
