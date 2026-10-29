# 38 · 多客户端 RTSP 入口统一设计

> 状态:**A1/A2/A3 已并入,批准 H1 开工**(单客户端行为等价替换)。B1-B8 评审项
> 已并入 §7a/§4.2/§4.3/§5/§6, 在 H3/H4 落地前生效即可。目标:把 `cam_rtsp.c`(多客户端,无事件)与 `evrec.c`
> (单客户端+事件录像)的 RTSP 服务端收敛为一个可复用模块,产品入口保持 `evrec`。

## 1. 现状:两套实现,各自合理但不可组合

| | `cam_rtsp.c`(909 行) | `evrec.c` 内置服务端 |
|---|---|---|
| 客户端模型 | **多客户端**:每连接一线程(`client_thread`),各自独立游标 `mine` | **单客户端**:第二个连接 accept 后立即 close |
| 帧分发 | 共享 `g_ring[RING]`(锁+cond),客户端从 `g_wr` 起读,落后太多跳到最旧可用 | 单投递环 `pkb[]`(深度 `--rtsp-ring`,默认 8),sender 单线程逐包投 |
| 慢客户端 | 跳帧(游标跳到 `g_wr-RING+1`),**不反压采集** | 非阻塞发送+poll 有界等待(单包 1s),判死连接,**不反压采集** |
| 事件录像 | 无 | 有(流式事件写入,docs/37) |
| 采集/编码 | 独立 `capture_thread`(v4l2 直采+MPP) | 主循环(DQBUF+MPP+RKNN+事件状态机) |
| 入口 | `cam_rtsp` 独立程序 | `evrec --rtsp` |

**问题**:两套 RTSP 状态机(DESCRIBE/SETUP/PLAY/RTP 打包/interleaved 写)是复制粘贴
关系,已开始漂移(evrec 的发送非阻塞化没回 port 到 cam_rtsp;cam_rtsp 的多客户端
游标模型 evrec 用不上)。任何 RTSP 层修一次要修两处。

## 2. 产品语义(先钉死)

| 问题 | 决定 |
|---|---|
| 采集/编码是否因客户端数量变化? | **否**。采集+编码+事件状态机是单实例生产者,与客户端数完全解耦;客户端只消费编码后的包 |
| 新客户端从哪帧开始? | **从最新 IDR 起**(保证立即可解码);IDR 前的帧不补发 |
| 最大客户端数? | **`--rtsp-clients N`(默认 4)**;第 N+1 个连接在 RTSP 握手完成后以 453 Too Many Connections 拒绝(比 accept 即 close 友好,客户端能拿到明确错误) |
| 慢客户端后果? | 只丢**它自己**的帧(游标跳帧或投递环丢包);超过硬上限(连续 N 秒零消费)判定死连接主动断开;**绝不反压采集/编码/事件录像** |
| 事件录像与推流的关系? | **同源分流**:编码器出的每个包,一份给事件状态机(流式写入),一份进推流分发环。两条消费路径互不依赖,任何一方停/慢都不影响另一方 |

## 3. 目标架构

```text
                    ┌→ 事件状态机(流式写入, docs/37)─→ 事件文件
编码器出包(每包一次)─┤
                    └→ rtsp_hub(新模块)─→ client[0..N] 各自 cursor/ring
```

### 3.1 新模块 `rtsp_hub.c`(单一事实来源)

从两套实现合并提炼,提供:

```c
struct rtsp_hub;
struct rtsp_client;

int  rtsp_hub_start(struct rtsp_hub **h, const struct rtsp_hub_cfg *cfg);
/* cfg: port / max_clients / ring_depth(每客户端) / 非阻塞发送参数 /
       on_packet 回调(由编码侧注册, hub 不关心包内容, 只透传 key/len/pts) */
void rtsp_hub_stop(struct rtsp_hub *h);
void rtsp_hub_feed(struct rtsp_hub *h, const uint8_t *pkt, int len,
                   int key, uint64_t pts_ms);   /* 编码侧每包调用, 永不阻塞 */
int  rtsp_hub_clients(const struct rtsp_hub *h); /* 当前 PLAY 数(统计/上报用) */
```

内部结构:

- **一个分发环**(生产者写, 锁+cond),替代 evrec 的 `pkb[]` 与 cam_rtsp 的
  `g_ring` 两套;
- **每客户端一个消费线程 + 私有游标**(cam_rtsp 模型):PLAY 时快照
  `cursor = 最新 IDR 的 tag`,从那里读;落后超过环容量跳到最旧可用(跳帧计数
  累计,供统计);
- **RTSP 信令解析**(OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN)提炼为无状态函数,
  SPS/PPS 由 hub 缓存(编码侧 scan 回调喂入);
- **发送路径非阻塞**(evrec 模型移植, 替代 cam_rtsp 的阻塞 xwrite):
  MSG_DONTWAIT + poll 有界等待, 单包判死;
- **拒绝策略**:握手完成(SETUP 后)检查 `nclients >= max_clients` → 回
  `453 Too Many Connections` 再 close。

### 3.1a 环内 payload 所有权(A1,阻断项)

"分发环 + 多客户端 + 环满丢最旧"同时成立时,客户端线程在锁外持有环内指针、
生产者逐出该槽 → UAF。现 evrec `pkb` 靠"锁内摘走槽位置 NULL"实现**单消费者**
独占,N 消费者不成立。**决定:锁内 memcpy 到客户端私有缓冲**(无引用计数,
bug 面最小;量级:12fps×4 客户端×平均 40KB≈2MB/s,可忽略;4K IDR 200KB 瞬时搬运)。
不变量两条,code review 必查:

1. **锁外不得持有环内指针**(消费 = 锁内 memcpy 到私有 buf,出锁后只碰私有 buf);
2. **drop 分两种语义分开计数**:`drop_evicted`(被生产者逐出,客户端被动)与
   `skip`(客户端主动跳帧/追帧),统计与日志分开,不许合并成一个 `drop`。

### 3.2 `evrec` 接入(产品入口)

- 删除 evrec 内置的 ~300 行单客户端服务端,改调 `rtsp_hub`;
- `rtsp_hub_feed` 挂在编码取包点(与事件状态机并列,顺序无关);
- 事件录像路径**零改动**(docs/37 状态机不动);
- `--rtsp-clients` 新参数;`--rtsp-ring` 语义变为"每客户端环深"。

### 3.3 `cam_rtsp` 退役路径

`cam_rtsp` 改为链接同一模块的薄壳(采集线程 + hub),或直接标记 deprecated
由 evrec 全量替代——**倾向后者**(cam_rtsp 的存在只剩历史意义),单独一个
commit 做删除,可回退。

> **执行记录(2026-10-29, H6)**:已标 deprecated(源码头横幅 + main 启动警告 +
> Makefile 注释),保留一个版本待删;README 诚实清单的"RTSP 只支持单客户端"
> 同步改为多客户端口径。V7(2h)通过后进入实际删除。

## 4. 关键设计点(对应 review 四重点)

### 4.1 RTSP server 与采集/编码的边界

- `rtsp_hub_feed()` 是**唯一**耦合点,语义 = "拷贝一份包进分发环,永不阻塞":
  环满丢最旧(计 `hub_drop`),malloc 失败丢本包;**不持采集/编码锁、不做 IO、
  不做任何可能睡眠超过 μs 级的工作**;
- 事件状态机与 hub 之间无共享状态(各自持有包副本);
- 编码线程内 hub 相关工作只有 memcpy + 环推进 + cond_broadcast。

### 4.2 每客户端独立 cursor 与 GOP 缓存

- 新客户端 PLAY 时从**最新 IDR** 起推(GOP 缓存 = 分发环本身就是按 key 标记的
  环,定位即一次扫描);无需独立 GOP 缓存结构——环深 ≥1 个 GOP 即可
  (`--rtsp-ring` 默认提到 30,约 2 个 GOP@12fps);
- cursor 用绝对 tag(单调递增),跳帧/断连都不会污染其他客户端;
- 每客户端统计字段(B5,统一口径):`sent_pkts`(记**包**,不记帧——
  现在 evrec 打印的 `sent` 是帧, 证据表无法横比)/`sent_bytes`/
  `skip_frames`(主动追帧)/`evicted`(被生产者逐出, 被动丢)/
  `sock_dead`(发送判死)/`lag_ms`(cursor 落后)/`last_send_age_s`;
  hub 级:`in/drop_evicted/halfopen/reject453`。

### 4.2a 每客户端 RTP 状态(A2,阻断项)

"RTP 打包无状态函数"只对**帧→NAL 切分与 FU-A 分片**成立;以下状态必须
per-client,随 `rtsp_client` 结构持有:

- `seq`(16 位回绕):两客户端共用会让各自序号跳号,播放器判丢包;
- `ssrc`:每客户端独立随机生成(SETUP 时),不能共享;
- `marker`:必须落在**该客户端实际发送的**访问单元最后一个包上——客户端
  跳帧后跳过的是整帧,帧尾 marker 随最后一包自然补上;若跳到半帧则整帧跳,
  不出现"marker 丢失";
- `ts`(90kHz):由包 pts 换算(`pts_ms*90`),**跳帧时 ts 真跳变**——
  禁止按包计数累加(会把时间轴压平,播放器卡顿/加速)。

打包函数签名即文档化:`rtp_send_frame(fd, ch, frame, pts_ms, &c->seq)`,
状态全部经 `rtsp_client*` 传入,无任何 `g_` 全局。

### 4.3 最大客户端数与拒绝策略

- 握手完成后拒绝(453),不是 accept 后 close —— 客户端 ffmpeg/VLC 能拿到
  明确错误码,排查成本最低;
- 拒绝只看 PLAY 数,DESCRIBE/SETUP(未 PLAY)不占名额但计入
  `hub_halfopen`(防半开连接堆积):**halfopen ≤ max_clients+2,且 SETUP 后
  10s 内未 PLAY 主动 close**(thread-per-client 无超时 = 无上限线程创建+fd
  占用的 DoS 面,A3 阻断项)。

### 4.4 慢客户端隔离硬上限

- 发送:非阻塞 + poll(POLLOUT) 上限 1s/包(evrec 现行模型),超时判死;
- 追帧:cursor 落后 >环深 → 跳到最旧可用并 `drop++`(cam_rtsp 现行模型);
- **硬上限**:单客户端连续 `--rtsp-deadline-s`(默认 5)秒零成功发送 → 主动
  close(防止端口堆积半死连接);
- 以上三条都只影响该客户端;采集/编码/事件录像路径无任何感知(设计不变量,
  板测用"一个 STOP 挂起的客户端 + 采集 fps 不降"验证)。

## 5. 板测验收矩阵

| # | 场景 | 判据 |
|---|---|---|
| V1 | 2/4 客户端并发 ≥10 min(板上 ffmpeg 自拉或 PC ffprobe) | 全程 `clients=N` 稳定;**采集 fps 不降**;每客户端 `sent` 单调增 |
| V2 | 新客户端中途加入 | 从 IDR 起可立即解码;既有客户端不受影响 |
| V3 | 慢客户端(SIGSTOP 3s < deadline 5s,随后 SIGCONT) | STOP 期间其余客户端与采集 fps 不受影响;CONT 后该客户端**恢复推流**(deadline 内未被判死);若 STOP 超 deadline 则该客户端被主动断开、其余不受影响(两种结局都算通过,断开路径要留日志) |
| V4 | 第 N+1 客户端 | 收到 453,既有客户端不受影响 |
| V5 | 断开/重连循环 ×10 | 每次重连正常;fd 数回基线;无 D 状态 |
| V6 | 事件录像并存 | 推流期间事件照常分段落盘;hub 丢帧不影响事件完整性 |
| V7 | 24h 前置:V1 通过后跑 2h | 无 fd 泄漏、RSS 有界 |

## 6. 实施切分

| 步骤 | 内容 | 预估 |
|---|---|---|
| H1 | `rtsp_hub.c/h` 从 evrec 提炼(单客户端先行, 行为等价替换内置服务端) | 中 |
| H2 | evrec 切换到 hub, 删内置实现; 板测 T1/T2 等价回归(单客户端行为不变) | 小 |
| H3 | 多客户端(每客户端线程+私有 cursor+453 拒绝) | 中 |
| H4 | 非阻塞发送移植 + 硬上限断连 | 小 |
| H5 | 板测 V1-V7 | 2 板端窗口 |
| H6 | cam_rtsp 退役:**V1-V7 全过后**先标 deprecated 保留一个版本,同一 commit 更新 docs/35 与 README 对 cam_rtsp 的引用 | 小 |

## 7. 边界与既有决策

- **RTCP**:现状 = 客户端发来的 interleaved RTCP **读入即丢弃**,服务端
  **不发 SR**。保留此决策(H1 行为等价);H5 验收必须用两种客户端覆盖
  (ffmpeg + VLC 或 GStreamer)——"没有 SR"在不同播放器上表现不同,
  只用 ffprobe 通过不算覆盖(B1);
- **不做 UDP RTP**(TCP interleaved 已满足, UDP 增加大量状态);
- **不做 RTSP over TLS / 鉴权**(局域网演示场景, 如需在入口前加反代);
- **不做录制回放/点播**(RTSP 只做实时流);
- 单进程内 hub;不引入进程间总线。

## 7a. 补充语义(B1-B4/B7 评审项)

- **453 语义(B2)**:RFC 2326 的 453 是 "Not Enough Bandwidth",
  "Too Many Connections" 属非标复用(业界普遍)。本服务行为:不排队、
  不自动重试、响应带 `Retry-After: 5`;**453 计数是容量指标**,运维侧
  持续增长 = 客户端数超预算;
- **环深双上限(B3)**:`--rtsp-ring N`(包数,默认 30)与新增
  `--rtsp-ring-mb`(字节,默认 8)同时生效,**逐出按字节优先**——
  4K 下 30 包可能是 6MB,包数上限不构成内存上界;
- **WiFi 口径(B7)**:`--rtsp-clients` 默认 4 是**有线实测上限**
  (4×~1.1Mbps 实测码率);WiFi 先撞吞吐瓶颈,按吞吐下调;
  V1 在无线环境下失败**不判 hub 缺陷**;
- **link_recover epoch(B4)**:链路故障恢复后 `hub_epoch++`,客户端只接受
  当前 epoch 的 key 包起播(或恢复时清环)——防止新客户端从故障前的
  旧 IDR 起播看到时间断层。H3 实现时落地。
