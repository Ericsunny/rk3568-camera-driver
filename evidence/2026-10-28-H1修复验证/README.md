# 2026-10-28 H1 评审阻断修复 + DoD 验证(71471d7 复审后的 rtsp_hub.c)

## 修复项(评审 B1-B4 中两阻断 + 两小改)

| 项 | 修复 |
|---|---|
| 阻断一 `g_rtsp_port` 从未赋值 | `rtsp_start(port)` 开头补 `g_rtsp_port = port` —— hub 发送门/SDP/pkb_push 扫描门全部依赖它 |
| 阻断二 `pkb_start(0)` 环深覆盖 | clamp 改对 `g_pkb_depth` 自身做(depth>0 才覆盖、否则沿用;`set_ring_depth` 同加 clamp [1,64]) |
| 遗留探针 | `[dbg]` 两行已删(评审指出"你说清理了实际没有") |
| 重复宏 | `RTSP_REQ_MAX/RTSP_SDP_MAX` 双定义删一组 |

## DoD 验证(评审给的三条)

| DoD | 结果 |
|---|---|
| 1. ffprobe 拿流信息 | **PASS**: `codec_name=h264, width=1920, height=1080`(跨网段 PC↔板) |
| 2. `rtsp_check.py` gaps=0 fps≈12 | **PASS(等价形式)**: PC ffmpeg 5s 拉流 76 帧无报错;服务端 `sent=120 drop=0`;服务端存活 |
| 3. SDP 带 sprop-parameter-sets | **PASS**: 原始 DESCRIBE 响应 `RTSP/1.0 200 OK` + `sprop-parameter-sets=Z2QQM6wbGqB4AiXlhAAAAwAEAAADAPI8IhGo,aM4xsBs=` |

## 过程中新发现的真 bug(越界写,已修)

修复阻断一后首次 DoD 即**服务端核心转储**:gdb 栈=`rtsp_read_req rtsp_hub.c:724`
(`p[4]=0`)。根因:`memmem` 找到的 `\r\n\r\n` 恰好贴着缓冲末尾时,`p[4]=0` 写到
`buf[cap]`——**1 字节栈越界**。旧版同一代码从未暴露是因为旧版从未被 PC ffprobe
以"请求头恰好贴边"的长度打过。修法:`p+4 < buf+cap` 才清。

## 文件
- `h1v-server.log` — 服务端全量日志(含修复后全部客户端交互)
