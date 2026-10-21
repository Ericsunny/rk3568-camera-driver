# tools

## `iq_patch_smartrun.py`

给 Rockchip RK AIQ 的 IQ json 补上 AWB 的 `smartRun` 校准块。

**为什么需要**:RK3568 这套 BSP 里 `librkaiq.so` 是 2024-03 的 AIQ 6.0,而 `/etc/iqfiles`
下的 IQ 文件是 2022 年 schema(板上 36 个全部缺 `smartRun`)。库初始化 AWB 时,
`UpdateAwbCalibV2Para()` 发现 `smartRun.cfg` 的四组数组长度为 0,直接早退、不把数组
拷进策略,下一帧 `AwbPreProcV201()` 就拿 `NULL/NULL/len=0` 去调 `interpolation()` → 段错误:

```
AWB:E:lost smartRun para in json
Thread 9 "xc:GrpMsgThrd" received signal SIGSEGV
#0 interpolation(float const*, float const*, int, float, float*)
#1 AwbPreProcV201(...)
```

补上这 13 行 JSON 后,同一块板、同一个库:**600 帧 / 40.1 s 不崩**,AE 收敛到 Y≈47,
AWB 收敛到 U−1.5 / V−2.7。

**用法**

```bash
# 先体检(不修改):打印 enable 与四个 _len
python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json --check

# 打补丁,默认 enable=0(只补字段,不改变 AWB 行为)
python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json

# 想真的打开 luma 自适应阈值
python3 iq_patch_smartrun.py /etc/iqfiles/imx415_xxx.json --enable 1
```

脚本按文本插入(保留原文件排版,只增 13 行)、插入后重新 `json.loads` 校验、
重复执行幂等;遇到 `"autoExtPara"` 不唯一时拒绝写入,避免插错相机。

**注意**

- 补丁只改 IQ,不动库和内核;
- `enable` 开/关的 AWB 结果差异在测量噪声内(180 帧对照:U−1.5/V−2.7 vs U−1.6/V−2.8),
  所以默认 0 最保守;
- 换 IQ 文件(升级/换 sensor)后要重新打一次;
- 想回退:删掉 `autoExtPara` 里的 `smartRun` 对象即可(或从备份还原 IQ)。
