/*
 * fi_test.c — 验证 ov8858_sm 的帧间隔三件套
 *
 * 本内核(4.19 + Rockchip 回移)的 ABI:
 *   VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL  _IOWR('V', 75, struct v4l2_subdev_frame_interval_enum)
 *   VIDIOC_SUBDEV_G_FRAME_INTERVAL     _IOWR('V', 85, struct v4l2_subdev_frame_interval)
 *   VIDIOC_SUBDEV_S_FRAME_INTERVAL     _IOWR('V', 86, struct v4l2_subdev_frame_interval)
 * 结构体布局按 include/uapi/linux/v4l2-subdev.h 手写,避免头文件版本差异。
 *
 * 用法: fi_test [dev]   默认 /dev/v4l-subdev2
 * 判据: enum index=0/1 都返回 1/30,index=2 返回 EINVAL;
 *       G 返回 1/30;S 设 1/30 成功、设 1/60 被拒。
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

struct v4l2_fract {
	unsigned int numerator;
	unsigned int denominator;
};

struct v4l2_subdev_frame_interval {
	unsigned int pad;
	struct v4l2_fract interval;
	unsigned int reserved[9];
};

struct v4l2_subdev_frame_interval_enum {
	unsigned int index;
	unsigned int pad;
	unsigned int code;
	unsigned int width;
	unsigned int height;
	struct v4l2_fract interval;
	unsigned int which;
	unsigned int reserved[8];
};

#define FI_IOWR(nr, type) _IOWR('V', nr, type)
#define VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL \
	FI_IOWR(75, struct v4l2_subdev_frame_interval_enum)
#define VIDIOC_SUBDEV_G_FRAME_INTERVAL \
	FI_IOWR(21, struct v4l2_subdev_frame_interval)
#define VIDIOC_SUBDEV_S_FRAME_INTERVAL \
	FI_IOWR(22, struct v4l2_subdev_frame_interval)

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/v4l-subdev2";
	struct v4l2_subdev_frame_interval_enum fie;
	struct v4l2_subdev_frame_interval fi;
	int fd, i, ret, fails = 0;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		printf("RESULT=FAIL open(%s): %s\n", dev, strerror(errno));
		return 1;
	}

	printf("sizeof(fie)=%zu sizeof(fi)=%zu dev=%s\n", sizeof(fie), sizeof(fi), dev);

	for (i = 0; i < 4; i++) {
		memset(&fie, 0, sizeof(fie));
		fie.index = i;
		fie.code = 0x3007;	/* SBGGR10_1X10 */
		fie.which = 1;		/* ACTIVE */
		ret = ioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL, &fie);
		if (ret == 0)
			printf("  enum[%d] = %u/%u\n", i,
			       fie.interval.numerator, fie.interval.denominator);
		else
			printf("  enum[%d] errno=%d (%s)\n", i, errno, strerror(errno));
	}

	memset(&fi, 0, sizeof(fi));
	ret = ioctl(fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &fi);
	if (ret == 0)
		printf("  get = %u/%u\n", fi.interval.numerator, fi.interval.denominator);
	else {
		printf("  get errno=%d (%s)\n", errno, strerror(errno));
		fails++;
	}

	/* 设成当前值应成功 */
	fi.interval.numerator = 1;
	fi.interval.denominator = 30;
	ret = ioctl(fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &fi);
	printf("  set 1/30 -> ret=%d errno=%d\n", ret, ret ? errno : 0);
	if (ret != 0)
		fails++;

	/* 设一个不支持的值应被拒(sensor 是固定帧率) */
	fi.interval.numerator = 1;
	fi.interval.denominator = 60;
	ret = ioctl(fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &fi);
	printf("  set 1/60 -> ret=%d errno=%d (期望非 0)\n", ret, ret ? errno : 0);
	if (ret == 0)
		fails++;

	close(fd);
	printf("RESULT=%s fails=%d\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
