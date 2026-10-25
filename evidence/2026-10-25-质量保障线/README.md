# 2026-10-25 质量保障线证据

内核 `4.19.232 #47`(md5 `0eee16fc…`),板 YCKJ iTOP-3568。生成命令与会话过程见各文件内注释。

## 文件清单

| 文件 | 内容 | 判定 |
|---|---|---|
| `板端证据-静态项.txt` | uname/内核 md5、`enum_mbus_code`(v4l2-ctl)、`enum_frame_size` 回归、TEST_PATTERN ctrl 与彩条实测列段、suspend 双路径结果、last_stop_err | 全部通过(详值见文件) |
| `soak2h-samples.csv` | 2h 服务级长稳(evrec 1080p+RTSP+90s 周期事件),60s 一行 RSS/线程/fd/温度/事件数 | **无泄漏**:119 个有效样本 RSS 21.7~43.6MB 有界波动(首行 256kB/fd=3 是采样与进程启动竞态的已知伪值),fd 恒 14,温度 40.6~46.1°C,80 个事件全部落盘 |
| `fv_ab_input_only.json` | **输入 native ab(108 帧)**:`mode=native, out_diff_bytes=0, det_mismatch=0, const_out_frames=0, err=0` | **输入 native 字节级一致的原始证据**(板上 `npu_ab300.json`,输入 native 时代二进制) |
| `fv_copy_baseline_input_era.json` / `fv_native_input_only.json` | 同期对照:拷贝基线(`cpu_copy_mb=83.203`,仅输入计数口径)与纯 native(`cpu_copy_mb=0.000`) | 输入拷贝计数归零的原始证据 |
| `fv_ab.json` / `fv_ab2.json` | **native output 尝试**(未入库代码):输入+输出全 native,ab 双路径对比 | **字节级验收不通过**:`out_diff_bytes` 6.67MB/6.73MB(~5%/帧,116 帧),`det_mismatch` 0→2,`const_out_frames=0`;第二次加了 `rknn_mem_sync(FROM_DEVICE)` 无改善→**排除 cache 一致性**,疑 runtime `outputs_get` 路径含 requant/布局转换。已回退到输入 native(字节级验证见 `fv_ab_input_only.json`) |

> **勘误**:soak2h `samples.csv` 末行写"# soak 结束: 样本 7200 行"——**7200 是运行秒数(脚本 `i` 变量),不是采样行数**;实际采样 120 行(含伪值首行)。该行为历史 CSV 保留不改;脚本已修复为分开打印"有效样本 N 行, 运行 M 秒"。

## 当前零拷贝口径(与 README/docs/31 一致)

- `zerocopy/` 实验链路:输入+输出全 native(mode C)
- `camd --npu-native`:**仅输入 native**(ab 108 帧字节级一致);输出 native 尝试见上表,回退
- `evrec`:全拷贝路径
