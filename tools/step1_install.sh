#!/bin/bash
# step1_install.sh -- 把新编的 ov8858_sm.ko 装进 /lib/modules 并提示重启
# 用法(板上): bash /root/step1_install.sh
# 说明:不要用 rmmod 热换 —— 我们在 2026-10-21 试过,rmmod 时 rkcif 还绑定着
#       这个 subdev,拆卸 media entity 会死锁,整机挂死(串口只剩回显)。
#       正确做法:装到 /lib/modules 后 reboot,让新模块在干净启动时加载。

set -eu

KO_SRC="${1:-/root/ov8858_sm_new.ko}"
KREL="$(uname -r)"
EXTRA="/lib/modules/${KREL}/extra"

echo "=== 目标 ==="
echo "kernel: ${KREL}"
echo "source: ${KO_SRC}"
echo "dest  : ${EXTRA}/ov8858_sm.ko"

test -f "${KO_SRC}" || { echo "找不到 ${KO_SRC}"; exit 1; }

# 2026-10-21 那次带 enum_frame_size / compat_ioctl32 的构建产物md5;
# 和你手上的 ko 不一致说明装的是旧文件,别急着重启。
EXPECT_MD5="${2:-c3b11d69e0dfdb476334ffa30351964f}"
ACT_MD5="$(md5sum "${KO_SRC}" | awk '{print $1}')"
echo "md5   : ${ACT_MD5} (期望 ${EXPECT_MD5})"
if [ "${ACT_MD5}" != "${EXPECT_MD5}" ]; then
	echo "!! md5 不符:确认这个 ko 是不是最新构建(继续安装,但重启后 step2 会对不上号)"
fi

mkdir -p "${EXTRA}"
cp -f "${KO_SRC}" "${EXTRA}/ov8858_sm.ko"
depmod -a

echo "=== 校验 ==="
md5sum "${KO_SRC}" "${EXTRA}/ov8858_sm.ko"
modinfo ov8858_sm | grep -E "^(filename|vermagic)" || true
ls -la "${EXTRA}/ov8858_sm.ko"

echo
echo "OK。现在执行:  reboot"
echo "重启后再跑:   bash /root/step2_verify.sh"
