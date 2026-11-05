# 2026-11-05 RAW10 离线统计:合成数据测试

- 环境:编译 VM,x86-64 Linux,Python 3.8.10;未连接板子,未使用历史证据帧。
- 命令:`python3 tools/tests/test_raw10_stats.py`;退出 0,原始输出见 [synthetic-test.log](synthetic-test.log)。测试自行在临时目录生成合成输入,覆盖已知 packed 低位字节序、四种 Bayer 相位、奇数 ROI、padding、多帧时域统计、u16 和异常输入等。
- 本地与 VM 源码 SHA-256 一致:`tools/raw10_stats.py` = `0df0bb7ffb557db19e41893f003c0bcadfb4f5c8bf014543917711e22e976cd4`;`tools/tests/test_raw10_stats.py` = `68157aa3215dcbbf903dbd1c2c11740d5019bd1e82450591eaba58121f4cc449`。
- 这是软件计算对合成已知输入的验证,不是 OV8858 文件格式确认、真实黑电平/噪声/坏点测量或 IQ 调优。实际格式和采集条件必须独立核对,见 [工具说明](../../tools/raw10_stats.md)。
