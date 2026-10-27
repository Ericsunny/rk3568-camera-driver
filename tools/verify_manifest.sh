#!/bin/sh
# verify_manifest.sh —— 板端核对版本清单(与 make_manifest.sh 生成的期望值对表)
# 用法: 在板上执行 sh verify_manifest.sh ;全部一致退出 0
set -e
FAIL=0
chk() { # chk <路径> <期望md5前8位或全值>
  if [ ! -f "$1" ]; then
    echo "MISS  $1"; FAIL=1; return
  fi
  got=$(md5sum "$1" | cut -d' ' -f1)
  case "$got" in
    "$2"*) echo "OK    $1 ${2:0:8}..";;
    *) echo "DIFF  $1 期望 $2 实得 $got"; FAIL=1;;
  esac
}

echo "== 板端资产核对 =="
chk /boot/Image-4.19.232        04672f71fffb580fe7c9b48a61b81e2d
chk /boot/Image-4.19.232.factory ed86c707df10b7bc5ef0637f6d12eed9
chk /boot/rk356x-yckj.dtb       ba47c41c287ac174e6532f8235ded06e
chk /root/npu/yolov8n_rk3568_i8.rknn 94fde17f2cde08b4f22c022b8674f116
chk /root/Image.p51             48b72b8dc231db2147430a74a603541e
chk /root/Image.p52             04672f71fffb580fe7c9b48a61b81e2d

if [ "$FAIL" = 0 ]; then echo "MANIFEST: ALL OK"; else echo "MANIFEST: MISMATCH"; exit 1; fi
