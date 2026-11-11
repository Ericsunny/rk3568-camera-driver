# BSP 补丁

这些补丁针对项目使用的 Rockchip Linux BSP。驱动内建于内核，应用补丁、构建 Image、部署和设备验证是不同步骤。使用前核对目标文件及上下文。

## ISP v21 params 缓冲大小

`isp21-params-buffer-size.patch` 修改 `isp_params_v21.c` 的参数缓冲分配大小：从 `sizeof(struct isp2x_isp_params_cfg)` 改为该文件实际解析的 `sizeof(struct isp21_isp_params_cfg)`。

本板曾出现 `G_FMT.buffersize=10475`，而 `QUERYBUF.length=10402`；mini3a 的长度检查因此拒绝提交。补丁修正分配端，不缩小用户态结构、不绕过长度检查。此前 BSP 的通用 params 格式查询已包含 v21 大小修正，该依赖也需核对。

```sh
cd /path/to/kernel
patch --dry-run -p1 < /path/to/isp21-params-buffer-size.patch
patch -p1 < /path/to/isp21-params-buffer-size.patch
```

完整构建、镜像指纹及板端结果见[事件服务验证记录](../../evidence/2026-11-08-event-service/README.md)。不能把该修复扩大解释为完成全部 ISP/IQ 调优。

## CIF 空指针检查

`rkcif-null-check.patch` 是独立的 CIF 防御性补丁。其检查对象和适用范围以补丁上下文与[内核问题记录](../../docs/stability/kernel-issues.md)为准，不能因为另一个补丁已部署就推断它也已部署。

## IMX415 可选全局表

`imx415-optional-global-regs.patch` 区分两通道模式可省略的 global 表和不能缺失的模式表：跳过前者，后者为空时返回 `-EINVAL`，实际 I²C 写失败继续返回。原源码版本、构建及 #62 板测见[修复记录](../../evidence/2026-11-10-imx415-drc-review/README.md)。补丁面向该厂商 BSP；先对目标源码 `patch --dry-run -p1` 检查上下文。

补丁应用后，可在 Linux 主机用真实 BSP 源码运行函数级替身测试：

```sh
python3 tools/tests/test_imx415_optional_global.py /path/to/kernel/drivers/media/i2c/imx415.c
```

此测试不访问真实相机，也不代替内核编译和板测。
