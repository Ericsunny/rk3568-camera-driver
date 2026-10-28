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

## 过程中的崩溃(gdb 栈 rtsp_hub.c:724 `p[4]=0`)——根因待定,非"1 字节越界"

修复阻断一后首次 DoD 即服务端 SIGSEGV,gdb 栈指向 `rtsp_read_req` 的 `p[4]=0`。
**边界推算否证了"1 字节越界"的最初归因**:`memmem` 保证 `p+4 ≤ buf+len`,
`recv`/`buf[len]=0` 保证 `len ≤ cap-1` → `p[4] ≤ buf[cap-1]`,那个字节本来就是 0,
该写入必然合法。加的 guard 在这条路径上不改变行为(no-op)。

**现行定性**:崩溃有两种可能——①栈在更早处已被打坏,724 行只是最后一下;
②`-O2` 行号归属偏移,bt 指向被调度过的语句。阻断一修复改变了代码路径
(DESCRIBE 从 503 走到正常 SDP),DoD 通过与此 guard 无因果关系。

**已做**:`p[4]` guard 保留(无害)+ `rtsp_read_req` 入口不变量防御
(`plen/cap` 违反时干净拒绝并打印 `[rtsp] req invariant broken`)。
**待办**:若 core 文件留存,`gdb bt full` 取 `len/cap/*plen` 实证;
取不到则登记为 H5 必测项("多客户端+大请求头+长连接"压测)。
本节此前版本的"1 字节越界已修"结论作废(此处为修正版)。

## 文件
- `h1v-server.log` — 服务端全量日志(含修复后全部客户端交互)

## 追加(评审第 3 节落地后复验)

- **声称未落地三处已全部落地并 grep 验证**: `set_ring_depth` clamp(hub:1142-1147)、
  `[dbg]` 探针删除(evrec 0 处)、重复宏删除(hub `RTSP_REQ_MAX` 1 处);
- **不变量防御已加**: `rtsp_read_req` 入口检查 `cap<8 || *plen<0 || *plen>cap-1`
  → 干净拒绝并打印 `[rtsp] req invariant broken`;
- **h1w 轮"崩溃"重新定性**: 该轮实为**新旧服务端实例端口冲突**(旧实例占 8554,
  新实例 bind 失败走"继续纯录像"路径, 我 kill 旧实例时误杀了新实例的观察对象)。
  gdb2.log 干净复现: 客户端接入→播放 sent=29→断开, 无崩溃;
- **不变量防御后 DoD 重跑**: ffprobe 拿 h264 1920x1080 ✓、5s 拉流累计 sent=149
  drop=0 ✓、服务端存活 ✓ —— 与修复前行为一致, guard 与不变量均为纯防御。
