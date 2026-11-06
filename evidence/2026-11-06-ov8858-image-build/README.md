# OV8858 当前源码：VM 增量内核构建

2026-11-06 在编译 VM 的隔离副本中，将当前仓库 OV8858 驱动源码加入现有内核树，增量构建 AArch64 `Image`。这是构建验证，**未部署候选镜像，也未用它做板测**。

## 输入与方法

- 仓库基线 `a160ad5d71d17cb66d533a189d9a9e2a214fccd7`；驱动 C 的 SHA-256 为 `0d4e2915eaa81ab4f720266ec65dc52ce49b70f447e4a8903e1ce6693f26b87f`，寄存器头文件 SHA-256 为 `93eb2b4d2d85d8295fcf1ebce2ef0aa6da75d8fad5f6be129e0637c84cd03492`。本地与 VM 复制品逐项核对一致。
- VM 原内核树为 `~/rk3568/RK_SDK/kernel`，配置 SHA-256 `3f54fd62c30da1d8d47aa31cf46b38ee0114a64cbcbb2a2d31a276180104f068`，`CONFIG_VIDEO_OV8858_SM=y`。复制内核树及已有构建产物到独立目录，排除 `.git`，仅在副本中替换上述驱动文件；原树未修改。
- 使用 VM 的 Buildroot GCC 9.3.0，命令为 `make -j4 ARCH=arm64 CROSS_COMPILE=<toolchain>/bin/aarch64-rockchip-linux-gnu- Image`，并将该工具链的 `lib` 加到本进程 `LD_LIBRARY_PATH`。[构建日志](build.log)、[开始](build-start.txt)、[结束](build-end.txt)、[退出码](build-exit.txt)均已保留；复制后的日志 SHA-256 与 VM 一致。
- 这是**增量构建**：副本包含原树既有的编译产物，不是干净全量重编译。日志显示重新编译了 `drivers/media/i2c/ov8858_sm.o` 并完成 `vmlinux` 链接及 Image 生成；`nm vmlinux` 可见 `ov8858_sm_probe/get_fmt/set_fmt` 符号。

## 结果与限制

- `make` 退出码 0，候选 Image SHA-256 `dda9e8f25072262e24fbba6200057f1f643c8340bfdf900dc03566f02cf37d34`，MD5 `7ea4629e15fb46f4880fda979d4b2dff`。候选保留在 VM 的隔离目录，未复制到板端。
- 构建日志有一行 `fatal: not a git repository (or any of the parent directories): .git`；构建仍继续并返回 0。隔离副本没有复制 `.git`，这行不能当成不存在，也不等于已证明无其他潜在构建问题。
- 原 VM 内核树相对其 Git 基线有大量未提交差异，`git diff --stat` 显示 512 个已跟踪文件变化，包括 ISP、传感器和设备树文件。它们未逐项与现役板端镜像对应，候选镜像不能被描述成仅含 OV8858 一处差异，也不宜直接部署。
- 同日只读查询：板端运行 `4.19.232 #52`，`/boot/Image-4.19.232` MD5 `04672f71fffb580fe7c9b48a61b81e2d`，与候选不同。文件哈希与运行版本的对应仍需结合引导路径确认；`rkaiq_3A_server` 当时运行中。
- 同日后续只读查询：`/boot` 所在分区显示可用约 12 MB，现役 `Image-4.19.232` 和另外两个 Image 文件各约 25 MB，`extlinux.conf` 指向 `/Image-4.19.232`。空间不足以按普通做法并排放入约 25 MB 的新镜像后再原子切换；没有删除、覆盖或调整任何文件。

## 排除额外的 IMX415 源码改动后重建

- 在 VM 找到较早的隔离内核副本，其中保存有 MD5 同为 `04672f71fffb580fe7c9b48a61b81e2d` 的 `codex-base52.Image` 文件；**这不自动证明该副本全部源码与 #52 的构建输入一致**。
- 将该历史副本作为源、VM 树作为目标，进行限于 `.c`、`.h`、`.S` 和 `Makefile` 的单向 rsync 校验比较，结果见 [比较输出](pm-vs-main-source-rsync.txt)。除生成头和跳过的符号链接外，可见差异包括 OV8858 驱动与 `imx415.c`；该比较不检查仅存在于目标中的文件，也不覆盖全部构建输入。
- `imx415.c` 的差异是在 1080p 表中启用 `0x301C=0x04` 寄存器项，与当前 OV8858 修改无关。第一版 Image 被保留；将隔离构建副本的 `imx415.c` 恢复为该历史副本的内容（SHA-256 `bc6eef3d1f4bd6bae7673feda5423ac796ce0d1959e15c0b22caa92f7162d54b`），再次增量构建。原 VM 内核树未修改。
- [重建日志](rebuild.log)显示 `imx415.o` 重编、`vmlinux` 重新链接及 Image 生成，[退出码](rebuild-exit.txt)为 0；仍有因隔离副本未含 `.git` 的同一条 `fatal` 提示。重建 Image SHA-256 `b7868e973e1c3ea419789a455dd574c450de690cbe92e982199354fb84bde10f`，MD5 `7bd5b38a041d275a4fdd2f115a675bfa`；日志及时间文件传输前后 SHA-256 一致。
- 这一步排除了**已发现的**额外 IMX415 源码项，不能证明候选与板端 #52 除 OV8858 外完全相同，也不替代部署和板测。两个 Image 均留在 VM 隔离目录，未部署。

下一步如需部署，应先审阅候选源码与现役镜像的来源差异，准备备份与恢复方案，并在得到明确授权后逐项执行覆盖和板测。历史实验与本次构建互不替代。
