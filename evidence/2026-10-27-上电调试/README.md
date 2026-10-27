# 2026-10-27 上电调试与 #52 修复验证

本轮从"疑似板级硬件间歇故障"查起,最终定位为**我方 #49/#51 引入的过度防御**,修复(#52)上板验证通过。
本目录是该过程的原始证据。

## 1. 结论

- **启动失败根因(已修复)**:`ov8858_sm` 的 `get_fmt/set_fmt` 对 **ACTIVE** 路径也做 `pad != 0 → -EINVAL`,
  而本 BSP 的 rkcif 把**流编号当 pad** 传入(`get_input_fmt(..., stream->id, ...)`,video0~3 → pad 0~3)。
  流 1/2/3 被拒 → `get_input_fmt()` 打 `sensor fmt invalid` 并返回 NULL → `rkcif_set_fmt()` 不查空解引用 →
  PID 1 死于 Oops → `Kernel panic`(开机 1.9 s)。
- **"时好时坏"** = 条件确定性:摄像头 probe 成功 → 必 panic;probe 失败 → 干净引导。
  video0 = pad 0 被放行,所以只用 video0 的历史测试从未暴露。
- **修复(#52,Image md5 `04672f71`)**:ACTIVE 不再拒非 0 pad(对齐同 SDK `imx415` 约定,`dev_dbg` 留痕);
  TRY 路径 `!cfg || pad != 0 → -EINVAL` 原样保留;`enum_*` 未动。

## 2. 验证结果

| 项 | 结果 | 证据文件 |
|---|---|---|
| 摄像头在位 + #51 | **3/3 确定性 panic** | `boot3-camera-in-panic.log` |
| 摄像头在位 + #52 冷启动 | **15/15 PASS**(`Detected OV8858`、无 `sensor fmt invalid`、无 `rkcif_set_fmt`、到 `login:`) | `cold-boot-ledger.csv`(逐次时间戳)、`cold-boot-1..4.log`、`cold-boot-15.log`、`boot-52-camera-ok.log`、主串口日志 |
| 摄像头拔掉 + #52 冷启动 | PASS(变量分离) | `boot-52-nocamera-ok.log` |
| 45 分钟连续出流(BG10 1632×1224) | **81054 帧 / 30.02 fps / 最大帧间隔 61 ms / ret=0 / 内核错误 0** | `soak_cif_45m.log` |
| `media-ctl -p` ×3 | rc=0、零 Oops、拓扑 12 条 ENABLED | `media-topology-p52.txt` |
| A0 只读基线 | `/boot` 三件套 md5、温度、eMMC 错误计数 0、工具清单 | `A0-只读基线体检.md` |

**边界(精确口径)**

- 15 次冷启动**都是同一块板**(YC_RK3568_MB1V0),不是多板统计;
- 45 分钟出流是**单次长跑**(TOTAL 行:`frames=81054 elapsed=2700.0s avg_fps=30.02 maxgap=0.061s ret=0`),不是多次重复的分布;
- 这 15 次只覆盖"摄像头在位 + 本内核"这一种故障模式,**不代表板级硬件(供电/eMMC)已全面排除**;
- 合规检查是**部分通过**(5 succeeded / 2 failed / 2 warnings),且发现厂商 `vb2_dma_sg` Oops,不能写成"compliance 通过"。

## 3. 合规检查发现(厂商栈缺陷,与本修复无关)

在 #52 上受控跑 `v4l2-compliance -m /dev/media0 -s -f`:

- rkcif 设备汇总:**5 succeeded / 2 failed / 2 warnings**(失败项为厂商 rkcif 的 legacy crop/compose
  与 `REQBUFS/CREATE_BUFS` 非法参数未返回 `EINVAL`,属既有归因);
- **新发现的内核 Oops**:`v4l2-compliance` 用非法格式/平面参数调 `VIDIOC_REQBUFS` 时,
  厂商 `vb2_dma_sg_alloc()` 直接进 `__sg_alloc_table_from_pages()` 建表,
  在**页数组为空**的情况下解引用 `NULL+0x10`:

  ```
  __sg_alloc_table_from_pages+0xd4
    sg_alloc_table_from_pages
      vb2_dma_sg_alloc+0x278        ← 厂商 videobuf2-dma-sg
        __vb2_queue_alloc → vb2_core_reqbufs → v4l_reqbufs
  ```

  完整栈见 `oops_reqbufs_compliance.txt`,工具原始报告见 `compliance_0618_0521.txt`。
- **后果**:该 Oops 把 `v4l2-compliance` 留在 **D 状态**(`vb2_fop_release` 不可中断睡眠),
  `kill -9` 无效,`/dev/video0` 被占住,**只能重启恢复**。
- **定性**:这是厂商 vb2-dma-sg + rkcif 参数校验缺口,属于**既有缺陷**;
  正常出流路径(45 分钟 81054 帧)不触发,与 #52 的 pad 修复无因果关系。
  记录在此,不在本轮修复范围内,也未改动厂商代码。

## 4. 文件索引

| 文件 | 内容 |
|---|---|
| `A0-只读基线体检.md` | 冻结现场时的只读体检记录 |
| `boot3-camera-in-panic.log` | #51 + 摄像头 的完整 panic 现场(NULL 解引用与调用栈) |
| `boot-52-camera-ok.log` / `boot-52-nocamera-ok.log` | #52 首次带/不带摄像头的启动日志 |
| `cold-boot-1..4.log` | 带摄像头冷启动抽样(串口原始段) |
| `serial-boot-full-raw.log` / `boot-t0-full.log` | 当日全量串口原始日志与 t=0 剥净版 |
| `soak_cif_45m.log` | 45 分钟连续出流统计 |
| `media-topology-p52.txt` / `dmesg-p52-camera.txt` | 拓扑与 dmesg 摘录 |
| `oops_reqbufs_compliance.txt` / `compliance_0618_0521.txt` | 合规检查发现的厂商 Oops 与工具原始输出 |

> 串口日志中的出厂默认口令已脱敏为 `<redacted>`。

## 5. 合规检查后的恢复(留档)

合规 Oops 导致 `video0` 被 D 状态进程占用、`kill -9` 无效,**断电重启后完全恢复**:

- 第 15 次带摄像头冷启动 PASS(`cold-boot-15.log`):`#52`、`Detected OV8858`、无 panic、到 `login:`;
- 重启后 `v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=30` **RC=0**,dmesg 内核错误 **0**,拓扑 **12 条 ENABLED**。

> 经验:D 状态(`vb2_fop_release`)进程不可杀,只能重启;`/tmp` 是 tmpfs,重启后临时工具需重传。
