/*
 * fi_probe.c — 探测内核/库之间 v4l2_subdev_frame_interval_enum 结构体大小是否一致
 *
 * 若某个尺寸返回 ENOTTY(而不是 EINVAL/0),说明该尺寸的 ioctl 编号内核不认识,
 * 即调用方(例如 2022 版 librkaiq)按另一种结构体大小发的请求会被内核直接拒绝。
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int probe(int fd, unsigned long nr, size_t sz)
{
	unsigned char buf[256];
	unsigned int cmd = _IOWR('V', nr, char[256]);
	int ret;

	/* 用真实尺寸重新构造命令号: _IOC(dir,type,nr,size) */
	cmd = 0xC0000000u | ((unsigned int)('V') << 8) | (unsigned int)nr |
	      (((unsigned int)sz & 0x3fffu) << 16);

	memset(buf, 0, sizeof(buf));
	/* index=0, which=1 (ACTIVE), code=0x3007 */
	*(unsigned int *)(buf + 0) = 0;	/* index */
	*(unsigned int *)(buf + 4) = 0;	/* pad */
	*(unsigned int *)(buf + 8) = 0x3007;	/* code */
	*(unsigned int *)(buf + 24) = 1;	/* which, 按 64 字节布局 */

	errno = 0;
	ret = ioctl(fd, cmd, buf);
	return ret == 0 ? 0 : -errno;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/v4l-subdev2";
	int fd = open(dev, O_RDWR);
	int s;

	if (fd < 0) {
		printf("open failed: %s\n", strerror(errno));
		return 1;
	}

	printf("enum_frame_interval(75) 各尺寸结果: 0=成功 -errno=被拒\n");
	for (s = 56; s <= 96; s += 8)
		printf("  size=%-3d -> %d\n", s, probe(fd, 75, (size_t)s));

	close(fd);
	return 0;
}
