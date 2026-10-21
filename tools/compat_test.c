/*
 * compat_test.c -- 验证 subdev 的 core.compat_ioctl32 是否真的接通
 *
 * 用法(在板上跑):
 *     ./compat_test /dev/v4l-subdevN          # 32 位二进制:走 compat 分支
 *     ./compat_test64 /dev/v4l-subdevN        # 64 位二进制:走 unlocked_ioctl
 *
 * 判据:
 *     32 位进程若驱动没实现 compat_ioctl32,内核返回 -ENOIOCTLCMD(-515);
 *     实现了就应打印 sensor/module/lens 三个字段(与 64 位一致)。
 *
 * 说明:
 *     RKMODULE_GET_MODULE_INFO 带一个定长结构体的用户指针,是最典型的
 *     "32 位指针必须 compat_ptr() 转换"的私有 ioctl,因此拿它来测最合适。
 *     结构体里没有任何指针/long,32/64 位布局相同,可以直接比较内容。
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/videodev2.h>	/* BASE_VIDIOC_PRIVATE, needed by rk-camera-module.h */
#include <linux/rk-camera-module.h>

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/v4l-subdev0";
	struct rkmodule_inf inf;
	int fd, ret;

	printf("ptr_bits=%zu sizeof(rkmodule_inf)=%zu dev=%s\n",
	       sizeof(void *) * 8, sizeof(inf), dev);

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		printf("RESULT=OPEN_FAIL errno=%d (%s)\n", errno, strerror(errno));
		return 1;
	}

	memset(&inf, 0, sizeof(inf));
	ret = ioctl(fd, RKMODULE_GET_MODULE_INFO, &inf);
	if (ret < 0) {
		printf("RESULT=FAIL errno=%d (%s)\n", errno, strerror(errno));
		close(fd);
		return 2;
	}

	printf("RESULT=OK sensor='%s' module='%s' lens='%s'\n",
	       inf.base.sensor, inf.base.module, inf.base.lens);
	close(fd);
	return 0;
}
