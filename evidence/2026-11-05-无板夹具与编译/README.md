# 2026-11-05 无板夹具与驱动编译记录

本次仅使用 x86-64 Ubuntu 编译 VM,未连接板子、未部署 Image、未执行采集或板端故障注入。证据保留本次命令输出及边界,不代表真实设备验收。

## 文件故障注入夹具

- [主机集成测试输出](fixture-test.log):VM GCC 9.4.0、Python 3,`python3 tools/tests/test_evrec_preload.py`,退出 0。测试从仓库当前 `evrec.c` 提取实际收尾函数及分支,在临时目录编译运行;普通/fortified 两种构建均覆盖正常对照、五种模拟故障及非目标路径,并检查符号链接、无命中、第二次调用和配置错误。该测试不是完整 evrec 或板测。
- [AArch64 交叉编译输出](fixture-cross-build.log):Buildroot GCC 9.3.0,库编译退出 0、无编译器警告;目标文件 SHA-256 `1e4bb2b3fcf701c60fcaf9016586afb804813247d67c2004e9e65db202f11dfd`。未在板端加载,libc/动态符号兼容性未验证。
- 过程中的环境问题:VM Linaro GCC 6.3.1 自带 sysroot 的 `pthread.h` 文件开头缺少 C 注释起始符,夹具交叉编译失败;备用 Buildroot GCC 初次运行因找不到宿主 `libisl.so.15` 失败,临时将其自带 `lib/` 加入该进程的 `LD_LIBRARY_PATH` 后成功。未修改原工具链或系统库。

## 当前 OV8858 驱动

- 驱动 C SHA-256 `0d4e2915eaa81ab4f720266ec65dc52ce49b70f447e4a8903e1ce6693f26b87f`;头文件 SHA-256 `93eb2b4d2d85d8295fcf1ebce2ef0aa6da75d8fad5f6be129e0637c84cd03492`。本地与 VM 隔离副本一致。
- VM 内核 `.config` SHA-256 `3f54fd62c30da1d8d47aa31cf46b38ee0114a64cbcbb2a2d31a276180104f068`;使用 VM 内核树和 Linaro GCC 6.3.1,在隔离目录生成外部模块模式对象。[Kbuild 原始输出](driver-kbuild.log),命令:`make -C /home/rk3568/rk3568/RK_SDK/kernel ARCH=arm64 CROSS_COMPILE=/home/rk3568/rk3568/RK_SDK/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu- M=/tmp/ov8858-current.fy32MeII ov8858_sm.o`,退出 0。
- [内建模式对象输出](driver-builtin.log):从 Kbuild 生成的编译命令中只去掉 `-DMODULE` 并更换输出文件名,使用同一配置与隔离源码重新编译,退出 0。这是对象级编译,**不是完整 Image 构建、链接、部署或板测**,也不证明配置与现役板端镜像一致。

## 源码指纹

本地和 VM 复制文件的 SHA-256 一致:

| 文件 | SHA-256 |
|---|---|
| `tools/evrec_fault_preload.c` | `f4f9a7da75ebc5ff3b79e1b4fb112485ba1c3a63fd9e116e1416d1625756aff7` |
| `tools/tests/test_evrec_preload.py` | `9589501fbf691930f0009be47268e0dd7dd7dd3d639f8aa05d699267a390ba55` |
| `tools/evrec.c` | `eb3349ef1b952275fb5b7abc4dd385f3294b6f5cd3631338f77214a54f069661` |

任何源码变化均需重跑测试并另增证据,不能复用这些哈希声称新版本已验证。
