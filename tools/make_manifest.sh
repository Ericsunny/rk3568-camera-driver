#!/bin/sh
# make_manifest.sh —— 生成"版本清单"manifest(可复现性的单一事实来源)
# 用法: sh tools/make_manifest.sh > docs/manifest.md   (或直接提交生成结果)
# 作用: 把仓库内所有非源码资产(模型/固件引用/二进制证据)与关键源文件的
#       md5 固化成清单; 板端对应用 tools/verify_manifest.sh 逐项核对。
set -e
cd "$(dirname "$0")/.."

echo "# 版本清单(manifest)"
echo
echo "- 生成时间: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "- 生成者: tools/make_manifest.sh(提交前重新生成)"
echo
echo "## 模型资产"
echo
echo '| 文件 | md5 | 用途 |'
echo '|---|---|---|'
for f in $(git ls-files '*.rknn' '*.onnx' 2>/dev/null); do
  echo "| \`$f\` | \`(见板端部署源,不入库)\` | NPU 推理 |"
done
[ -z "$(git ls-files '*.rknn' '*.onnx' 2>/dev/null)" ] && \
  echo '| (模型不入库; 板端部署源与 md5 见 evidence 对应 README) | | |'
echo
echo "## 源码指纹(驱动与工具)"
echo
echo '| 文件 | md5 |'
echo '|---|---|'
for f in driver/ov8858_sm/ov8858_sm.c tools/evrec.c tools/camd.c tools/st_burn.c \
         tools/gate_camera_boot.sh tools/check_3a_lifecycle.sh; do
  [ -f "$f" ] && echo "| \`$f\` | \`$(md5sum "$f" | cut -d' ' -f1)\` |"
done
echo
echo "## 板端关键资产(部署后用 verify_manifest.sh 核对)"
echo
echo '| 资产 | 期望值 | 来源 |'
echo '|---|---|---|'
echo '| /boot/Image-4.19.232 (#52) | \`04672f71fffb580fe7c9b48a61b81e2d\` | PR #1 验证版 |'
echo '| /boot/Image-4.19.232.factory | \`ed86c707df10b7bc5ef0637f6d12eed9\` | 出厂备份 |'
echo '| /boot/rk356x-yckj.dtb | \`ba47c41c287ac174e6532f8235ded06e\` | 项目 dtb |'
echo '| /root/npu/yolov8n_rk3568_i8.rknn | \`94fde17f2cde08b4f22c022b8674f116\` | NPU 模型 |'
echo '| /usr/lib/librknnrt.so | (2.3.2, 出厂版备份为 .bak-factory) | runtime 升级 |'
