# 版本清单(manifest)

- 生成时间: 2026-10-27 12:13:23 UTC
- 生成者: tools/make_manifest.sh(提交前重新生成)

## 模型资产

| 文件 | md5 | 用途 |
|---|---|---|
| (模型不入库; 板端部署源与 md5 见 evidence 对应 README) | | |

## 源码指纹(驱动与工具)

| 文件 | md5 |
|---|---|
| `driver/ov8858_sm/ov8858_sm.c` | `167c991b26f4b632b70b65a17fecfa74` |
| `tools/evrec.c` | `31d3b338d8858d6b2f83d7542e3f32e6` |
| `tools/camd.c` | `85afaad4458a0e8e214dd9d226217f8e` |
| `tools/st_burn.c` | `ae5c51e85c0ff75a2780a9a7f4886ecc` |
| `tools/gate_camera_boot.sh` | `525bf4341915bf98503e0b8a18b0214d` |
| `tools/check_3a_lifecycle.sh` | `ecfdad1197abd2f34bfa3ff1c3e81c61` |

## 板端关键资产(部署后用 verify_manifest.sh 核对)

| 资产 | 期望值 | 来源 |
|---|---|---|
| /boot/Image-4.19.232 (#52) | \`04672f71fffb580fe7c9b48a61b81e2d\` | PR #1 验证版 |
| /boot/Image-4.19.232.factory | \`ed86c707df10b7bc5ef0637f6d12eed9\` | 出厂备份 |
| /boot/rk356x-yckj.dtb | \`ba47c41c287ac174e6532f8235ded06e\` | 项目 dtb |
| /root/npu/yolov8n_rk3568_i8.rknn | \`94fde17f2cde08b4f22c022b8674f116\` | NPU 模型 |
| /usr/lib/librknnrt.so | (2.3.2, 出厂版备份为 .bak-factory) | runtime 升级 |
