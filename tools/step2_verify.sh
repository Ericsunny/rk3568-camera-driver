#!/bin/bash
# step2_verify.sh -- 重启后验证 enum_frame_size / compat_ioctl32,并跑 A/B
# 用法(板上): bash /root/step2_verify.sh
# 前置:step1_install.sh 已执行并 reboot(新 ko 在 /lib/modules/$(uname -r)/extra)

set -u
LOG=/root/verify_$(date +%Y%m%d_%H%M%S).log
exec > >(tee -a "$LOG") 2>&1
echo "日志: $LOG"
echo

echo "========== 0. 模块来源 =========="
lsmod | grep -i ov8858 || echo "(ov8858_sm 未加载!)"
EXTRA="/lib/modules/$(uname -r)/extra/ov8858_sm.ko"
ls -la "$EXTRA" 2>/dev/null
md5sum "$EXTRA" 2>/dev/null
dmesg | grep -i "ov8858" | tail -8

echo
echo "========== 1. 找 ov8858 的 subdev 节点 =========="
SUBDV=""
for f in /dev/v4l-subdev*; do
	[ -e "$f" ] || continue
	n=$(v4l2-ctl -d "$f" --info 2>/dev/null | sed -n 's/.*Driver name *: *//p')
	case "$n" in
		*ov8858*) SUBDV="$f"; echo "$f -> $n";;
	esac
done
if [ -z "$SUBDV" ]; then
	echo "!! 没找到 ov8858 subdev,后面 subdev 测试跳过"
else
	echo "SUBDV=$SUBDV"
fi

echo
echo "========== 2. enum_frame_size =========="
echo "--- (a) v4l-utils 是否支持 subdev framesizes ---"
v4l2-ctl --help-all 2>/dev/null | grep -i "framesize" | head -5

if [ -n "$SUBDV" ]; then
	echo "--- (b) 直接在 subdev 上枚举(pad 0) ---"
	for opt in \
		"--list-subdev-framesizes=0" \
		"--list-subdev-framesizes=0,$(v4l2-ctl -d "$SUBDV" --get-subdev-fmt=0 2>/dev/null | sed -n 's/.*Code *: *//p' | head -1)"; do
		echo "\$ v4l2-ctl -d $SUBDV $opt"
		v4l2-ctl -d "$SUBDV" $opt 2>&1 | head -20
	done
fi

echo "--- (c) 从 /dev/video0 侧看(--list-formats-ext 会走 subdev 的 enum_frame_size) ---"
v4l2-ctl -d /dev/video0 --list-formats-ext 2>&1 | head -40

echo
echo "========== 3. compat_ioctl32 =========="
if [ -n "$SUBDV" ]; then
	echo "--- 64 位(走 unlocked_ioctl)---"
	/root/compat_test64 "$SUBDV"
	echo "--- 32 位(走 compat_ioctl32,未实现时会 -ENOIOCTLCMD=-515)---"
	/root/compat_test32 "$SUBDV"
else
	echo "跳过:没有 subdev 节点"
fi

echo
echo "========== 4. A/B: rkaiq AWB vs mini3a AWB =========="
if [ -x /root/grab ] && [ -x /root/mini3a/mini3a ]; then
	python3 /root/ab_awb.py
else
	echo "跳过:缺 /root/grab 或 /root/mini3a/mini3a"
	ls -la /root/grab /root/mini3a/mini3a 2>/dev/null
fi

echo
echo "========== 完成 =========="
echo "日志已保存: $LOG"
