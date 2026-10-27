# 2026-10-27 RTSP 客户端行为实测(evrec 内置服务端,#52)

## 前提

`evrec 4K+检测+RTSP`(:8554)按 docs/858 行头注:**单客户端模式,第二个连接 accept 后立即 close**。
客户端=板上自带 `ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live -f null -`(真实 DEMUX+解码)。

## 结果矩阵

| 项 | 实测 | 判定 |
|---|---|---|
| 4 客户端并发 | 4 个 ffmpeg 只活 3 个(另 1 个握手后即断);服务端 `clients=1` 恒定 | **单客户端上限确认**(设计如此,非缺陷) |
| 第二连接被拒 | 第二个 ffmpeg 报 `Invalid data found`(服务端 accept 即 close),第一个不受影响 | 拒绝路径干净,无干扰 |
| 单客户端持续拉流 | `clients=1` 持续,`sent` 268→768 单调增长,`drop=0` | 稳定 |
| **慢客户端(零窗口)** | `kill -STOP` 挂起 ffmpeg 后,采集 **7.3 fps 纹丝不动**(阶段耗时序列无抖动),`drop=0` | **非阻塞发送+有界等待兑现,反压不传导到采集** |
| 挂起后恢复读 | `kill -CONT` 后 `sent` 606→709 继续增长 | 恢复正常 |
| 断开重连 | kill 客户端 → 新 ffmpeg 再连,`clients=1`/`sent` 恢复增长 | 正常 |
| 采集 fps 全程 | 7.0(启动期)→7.3,全部客户端事件前后**无下降** | 符合验收"fps 不下降" |
| 事件录像 | 36 个 h264+36 个 json 成对落盘,期间未中断 | 符合"录像不断" |
| D 状态进程 | 无(ps 清单为空) | 符合 |
| dmesg | `timeout/Unable to handle` 计数 0 | 干净 |

## 边界(口径)

- 本轮验证的是 **evrec 内置单客户端服务端**的行为边界;"多客户端并发"按设计**不支持**
  (第二个连接被拒),多客户端入口统一(`cam_rtsp` 改造)是后续独立任务;
- 客户端为 ffmpeg 到 null(解封装+解码,无渲染);真实播放器(VLC)与网络跨段未覆盖;
- `cam_rtsp.c` 的多客户端实现(每客户端一线程+独立 ring)未在本轮测试范围。

## 文件

- `rtsp_multi.log` — 服务端全量日志(749 行,含全部 clients/sent/drop 周期行与阶段耗时)
- `second-conn-rejected.log` — 第二连接被拒时 ffmpeg 的报错

## 跨网段实测补充(2026-10-27,PC ffmpeg 7.1 ↔ 板 192.168.10.2:8554)

| 项 | 实测 |
|---|---|
| SDP 探测(ffprobe) | h264 3840×2160,`r_frame_rate=43/6 ≈ 7.17 fps`——与采集侧 7.3 fps 一致 |
| 60s 单客户端拉流 | 成功;板端 `sent` 29→482,采集 7.3 fps 稳定 |
| 120s 长拉(客户端A) | rc=0 正常结束,期间 `clients=1`/`drop=0` |
| 并发第二连接(客户端B) | **被拒**(exit 183,`Invalid data/Connection reset`),A 全程不受影响 |
| 已知噪音 | ffmpeg null muxer 报 `non monotonically increasing dts`(等值 DTS,UDP/TCP 抖动下常见,非流损坏;不影响解码) |

**结论**:跨网段单客户端链路(真实网络+真实 PC 解码器)打通;单客户端边界与板内实测一致。
多客户端并发仍是未实现能力(按设计拒绝),入口统一为后续独立任务。

## 文件(补充)

- `netrtsp.log` — 本轮服务端日志
- `pc-clientA-120s.log` — 120s 长拉客户端日志(rc=0)
- `pc-clientB-rejected.log` — 第二连接被拒报错
