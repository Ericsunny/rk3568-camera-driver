# 2026-10-27 3A 生命周期复测(#52 内核)

目的:用 #52 内核复测 `docs/29 §10` 的 A1/A4/A2 三格,确认文档口径与当前内核一致。

## 环境

- 内核 `#52`(md5 `04672f71`),IMX415 在 J10(CAM0)、OV8858 在 J11(CAM1)同时在位;
- `rkaiq_3A` 开机自启 **active**(有 IMX415 时不再断言 `isp_hw_ver == 21`);
- `video5` = 3840x2160 NV12;`gate_camera_boot.sh "#52"` → **PASS**(ENABLED=12)。

## 结果

| 格 | 操作序列 | 结果 |
|---|---|---|
| **A1** | 3A active → `v4l2-ctl -d /dev/video5 --stream-count=60` | **PASS**:60 帧 @ 15.00 fps,RC=0,dmesg 无 timeout |
| **A4'**(新) | 3A active → 采 60 帧并干净 STREAMOFF → `systemctl stop rkaiq_3A` → 再采 | **FAIL(硬 wedge)**:`waiting on params stream on event timeout` → `rkisp_stream_stop id:0 timeout`;复采 20/10 帧均 RC=124、零帧;仅重启可恢复 |
| 隔离性 | wedge 后采 CIF 链 `/dev/video0` 3 帧 | **不受影响**,RC=0 → wedge 局限在 ISP 侧 |

原始 dmesg:`a4_wedge.txt`。

## 重要修正

`docs/29 §10` 的 A4 格(以及据此改写的 `docs/35 §4.1`)写的是"**干净停 3A 后不必重启即可采集**",
但本次复测证明**该结论有前提**:它来自 10-26 的实验序列(3A 启动后**未采过流**就直接停),
而"**3A 运行中采过流 → 干净 STREAMOFF → 再停 3A → 采集**"会触发
`waiting on params stream on event timeout` 并 wedge。

推测机制:rkisp 的 params(video13)流一旦被 3A 拉起过,停掉 3A 后新开 mainpath 仍等待
params stream on 事件(3A 已不在,没人提供),于是超时;若 3A 从未拉起 params 流,则不等待。

## 待验证(E1,需重启后做)

- **E1**:重启 → 3A(开机自启) **不采流**直接 `systemctl stop rkaiq_3A` → 采 60 帧。
  - 若 PASS → 规则成立:"**是否在本 boot 内拉起过 params 流**"才是 A4 的前提;
  - 若 FAIL → 说明 10-26 的 A4 结论在此内核上不成立,需要重测整格。

E1 完成前,文档保持"待复测"标注,不写成确定结论。

## E1 结果(2026-10-27,补测)

- 序列:重启 → 3A 开机自启 active(本 boot 内 `params stream on event` 计数 **0**,未采过流)→
  `systemctl stop rkaiq_3A`(procs=0)→ `v4l2-ctl -d /dev/video5 --stream-count=60`;
- 结果:**FAIL**,RC=124;dmesg `waiting on params stream on event timeout` → `rkisp_stream_stop id:0 timeout`。

即"3A 启动过但未采流 → 停掉 → 采"同样 wedge。**10-26 的 A4 之所以通过,是因为当时 `rkaiq_3A.service` 处于停用状态,3A 从未在本 boot 启动。**

## 源码级机制(VM 树实读)

```c
/* rkisp.c: rkisp_start_3a_run() */
if (!rkisp_is_need_3a(dev) || dev->isp_ver == ISP_V20 || !params_vdev->is_subs_evt)
        return;                       /* 3A 没订阅过 → 不等待,直接启动 */
v4l2_event_queue(vdev, &ev);
wait_event_timeout(dev->sync_onoff,
                   params_vdev->streamon && !params_vdev->first_params, 1000ms);
if (!ret) v4l2_warn(..., "waiting on params stream on event timeout");
```

```c
/* isp_params.c */
:78   case CIFISP_V4L2_EVENT_STREAM_START/STOP: params_vdev->is_subs_evt = true;   /* 订阅时置真 */
:90   rkisp_params_unsubs_evt():                params_vdev->is_subs_evt = false;  /* 只有显式 unsubscribe 才清 */
:399  probe():                                  params_vdev->is_subs_evt = false;
```

**结论**:`is_subs_evt` 在 3A **订阅时置真**,且**关闭 fd 不清**,只有显式 `VIDIOC_UNSUBSCRIBE_EVENT` 才清零。
3A 被 stop/kill 时通常不会走 unsubscribe ⇒ 标志永久留在 true ⇒ 本 boot 内之后每次开 mainpath 都会
等 params 流 1 s,等不到就超时,ISP 后继 STREAMOFF 也超时 → wedge。

## 修正后的模型(待 E2 确认)

| 前提 | 直接采集(不重启) |
|---|---|
| 3A **从未在本 boot 启动**(服务 disabled) | 预期 **可用**(`is_subs_evt=false`,不等待)——**E2 待验** |
| 3A 启动过,随后 stop/kill(无论是否采过流) | **必 wedge**(E1、A4' 两次实测) |

## E2 设计(已准备,等重启)

板上已 `systemctl disable rkaiq_3A`(当前 `disabled`/`inactive`)。断电重启后:

1. 确认 `systemctl is-active rkaiq_3A` = inactive(且未启动过);
2. `v4l2-ctl -d /dev/video5 --stream-count=60` → 预期 PASS(15 fps);
3. 完成后 `systemctl enable rkaiq_3A` 恢复默认(下一次 boot 再验 A1)。

## E2 结果(2026-10-27,已确认)

- 前提:`systemctl disable rkaiq_3A` 后重启 → `3A enabled=disabled active=inactive`、`params event` 计数 0;
- `v4l2-ctl -d /dev/video5 --stream-count=60` → **PASS**:15.00 fps,RC=0,dmesg 无 `params stream on event`/`rkisp_stream_stop`/`timeout`。

⇒ **"3A 从未在本 boot 启动"时,不重启直接采是安全的**;A4 成立的前提就是这一条。

## A2 复测(2026-10-27,修正旧表述)

- 序列:3A active → 后台长流 → `systemctl stop rkaiq_3A` → 观察 4 s;
- 结果:**在跑的流立即停止产出**——采集进程 CPU ticks 15 → 15(零增长),阻塞在 DQBUF;
  终止该进程 → `rkisp_stream_stop id:0 timeout`;新开流 **RC=124**(A5 复现)。
- **旧表述"在跑的流暂时无恙"作废**:停 3A 瞬间帧就停了,只是进程还挂着没报错。

## 最终模型(#52 实测,A1/A2/A4/A4′/E1/E2)

| 前提 | 直接采集 | 依据 |
|---|---|---|
| 3A 常驻(active) | **可用** | A1(60 帧 15.00 fps) |
| 3A 本 boot 从未启动(disabled) | **可用** | E2(60 帧 15.00 fps) |
| 3A 启动过 → stop | **wedge**(params 超时 + STREAMOFF 超时) | A4′、E1 |
| 采流中 stop 3A | **帧立即停** → STREAMOFF 超时 → wedge | A2 |

恢复手段只有重启。机制见 §源码级机制。
