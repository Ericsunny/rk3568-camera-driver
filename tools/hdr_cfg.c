// hdr_cfg: 切 IMX415 到 DOL X2 模式, 并读回确认
// 用法: ./hdr_cfg [dev] [mode]    mode: 0=NO_HDR 5=HDR_X2 6=HDR_X3
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/media-bus-format.h>
#include <linux/rk-camera-module.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/v4l-subdev6";
	int want = argc > 2 ? atoi(argv[2]) : HDR_X2;
	int fd = open(dev, O_RDWR);
	if (fd < 0) { printf("open %s: %s\n", dev, strerror(errno)); return 1; }

	struct v4l2_subdev_format f;
	memset(&f, 0, sizeof(f));
	f.pad = 0; f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &f) == 0)
		printf("before: %ux%u code=0x%04x\n", f.format.width, f.format.height, f.format.code);
	else
		printf("G_FMT: %s\n", strerror(errno));

	memset(&f, 0, sizeof(f));
	f.pad = 0; f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	f.format.width = 1944; f.format.height = 1096;
	f.format.code = MEDIA_BUS_FMT_SGBRG10_1X10;
	f.format.field = V4L2_FIELD_NONE;
	f.format.colorspace = V4L2_COLORSPACE_SRGB;
	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f) == 0)
		printf("S_FMT ok: %ux%u vc=%u\n", f.format.width, f.format.height, 0u);
	else
		printf("S_FMT: %s\n", strerror(errno));

	struct rkmodule_hdr_cfg cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.hdr_mode = want;
	if (ioctl(fd, RKMODULE_SET_HDR_CFG, &cfg) == 0)
		printf("SET_HDR_CFG(%d) ok\n", want);
	else
		printf("SET_HDR_CFG(%d): %s\n", want, strerror(errno));

	memset(&cfg, 0, sizeof(cfg));
	if (ioctl(fd, RKMODULE_GET_HDR_CFG, &cfg) == 0)
		printf("GET_HDR_CFG -> hdr_mode=%u esp=%d\n", cfg.hdr_mode, cfg.esp.mode);
	else
		printf("GET_HDR_CFG: %s\n", strerror(errno));

	close(fd);
	return 0;
}
