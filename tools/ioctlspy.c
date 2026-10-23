/*
 * ioctlspy.c — LD_PRELOAD 拦截 ioctl,记录 V4L2 subdev 帧间隔相关请求
 *
 * 目的: 2022 版 librkaiq 直连 /dev/v4l-subdev2 调 ENUM_FRAME_INTERVAL 失败,
 * 而我们的手写程序同样的设备能成功。怀疑是库编译期的结构体尺寸与本内核不同
 * (本内核按 _IOC_SIZE 严格匹配 ioctl 号,不匹配即 ENOTTY)。
 *
 * 记录内容: nr / size / 前 40 字节参数 / 返回值 / errno
 * 输出: 环境变量 SPY_OUT 指定的文件(默认 /tmp/ioctlspy.log)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>

typedef int (*ioctl_fn)(int, unsigned long, void *);

static ioctl_fn real_ioctl;
static FILE *out;
static int inited;

static int interesting(unsigned long cmd)
{
	unsigned int dir = (cmd >> 30) & 0x3;
	unsigned int type = (cmd >> 8) & 0xff;
	unsigned int nr = cmd & 0xff;

	if (dir != 0x3)		/* 只看 _IOWR */
		return 0;
	if (type != 'V')
		return 0;
	/* 74/75 = enum frame size/interval, 21/22 = g/s frame interval, 85/86 = 新版 */
	return nr == 74 || nr == 75 || nr == 21 || nr == 22 ||
	       nr == 85 || nr == 86 || nr == 76 || nr == 77;
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int ret, saved;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!real_ioctl)
		real_ioctl = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
	if (!inited) {
		const char *p = getenv("SPY_OUT");

		out = fopen(p ? p : "/tmp/ioctlspy.log", "a");
		inited = 1;
	}

	ret = real_ioctl(fd, request, arg);
	saved = errno;

	if (out && interesting(request)) {
		unsigned int *u = arg;
		unsigned char *b = arg;
		int i;

		fprintf(out, "cmd=0x%08lx nr=%lu size=%u ret=%d errno=%d arg=",
			request, request & 0xff, (unsigned)((request >> 16) & 0x3fff),
			ret, ret < 0 ? saved : 0);
		for (i = 0; i < 40; i++)
			fprintf(out, "%02x", b ? b[i] : 0);
		if (u) {
			fprintf(out, " idx=%u pad=%u code=0x%x w=%u h=%u",
				u[0], u[1], u[2], u[3], u[4]);
			/* 64 字节布局: interval 在 +24/+28, which 在 +32 */
			fprintf(out, " fract=%u/%u which=%u", u[6], u[7], u[8]);
		}
		fprintf(out, "\n");
		fflush(out);
	}

	errno = saved;
	return ret;
}
