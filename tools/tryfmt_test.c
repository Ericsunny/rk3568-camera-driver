/*
 * tryfmt_test.c —— 复现/验收:VIDIOC_SUBDEV_TRY_FMT 是否污染 ACTIVE 状态
 *
 * 用法:
 *   ./tryfmt_test [dev] [pad]              全量用例(8 个),判 PASS/FAIL
 *   ./tryfmt_test -o 1920x1080 [dev] [pad] 只做一次 TRY(用于复现脚本)
 *
 * 判据:
 *   1) 每次 TRY_FMT 之后,ACTIVE 的 G_FMT 必须与基线完全一致(宽/高/code);
 *   2) 全部用例通过 => RESULT=PASS,否则 RESULT=FAIL。
 *
 * 退出码: 0 = PASS, 1 = FAIL, 2 = 打不开设备/基线失败
 *
 * 背景: ov8858_sm.c 原 set_fmt 在判断 which 之前就先写了
 *   ov8858->is_1080p = (w==1920 && h==1080);
 * 于是任何一次 TRY_FMT(1920x1080) 都会把驱动全局模式切到 1080p,
 * 之后 G_FMT(ACTIVE) 报 1920x1080、出流也用 1080p —— TRY 本应"只试不改"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>

#define MBUS_CODE 0x3007 /* MEDIA_BUS_FMT_SBGGR10_1X10 */

static int g_fmt(int fd, int which, int pad, struct v4l2_mbus_framefmt *out)
{
	struct v4l2_subdev_format f;

	memset(&f, 0, sizeof(f));
	f.pad = pad;
	f.which = which;
	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &f) < 0)
		return -errno;
	*out = f.format;
	return 0;
}

static int t_fmt(int fd, int pad, unsigned int w, unsigned int h,
		 struct v4l2_mbus_framefmt *out)
{
	struct v4l2_subdev_format f;

	memset(&f, 0, sizeof(f));
	f.pad = pad;
	f.which = V4L2_SUBDEV_FORMAT_TRY;
	f.format.width = w;
	f.format.height = h;
	f.format.code = MBUS_CODE;
	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f) < 0)
		return -errno;
	*out = f.format;
	return 0;
}

static int same(const struct v4l2_mbus_framefmt *a,
		const struct v4l2_mbus_framefmt *b)
{
	return a->width == b->width && a->height == b->height &&
	       a->code == b->code;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/v4l-subdev2";
	int pad = 0;
	int only_w = 0, only_h = 0, a;
	int fd, i, changed = 0, ret, n;
	struct v4l2_mbus_framefmt base, cur, try_;
	static const unsigned int cases[][2] = {
		{ 1632, 1224 },
		{ 1920, 1080 },
		{ 1280, 720 },
		{ 640, 480 },
		{ 4000, 3000 },
		{ 1632, 1080 },
		{ 1920, 1224 },
		{ 800, 600 },
	};

	n = (int)(sizeof(cases) / sizeof(cases[0]));

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-o") && a + 1 < argc) {
			if (sscanf(argv[++a], "%ux%u", &only_w, &only_h) != 2) {
				printf("RESULT=ERROR bad -o, want WxH\n");
				return 2;
			}
		} else if (argv[a][0] == '/') {
			dev = argv[a];
		} else {
			pad = atoi(argv[a]);
		}
	}

	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("RESULT=ERROR open %s: %s\n", dev, strerror(errno));
		return 2;
	}

	ret = g_fmt(fd, V4L2_SUBDEV_FORMAT_ACTIVE, pad, &base);
	if (ret) {
		printf("RESULT=ERROR baseline G_FMT: %s\n", strerror(-ret));
		close(fd);
		return 2;
	}
	printf("=== baseline (ACTIVE) ===\n");
	printf("ACTIVE: %ux%u code=0x%04x\n", base.width, base.height, base.code);

	if (only_w && only_h) {
		ret = t_fmt(fd, pad, only_w, only_h, &try_);
		if (ret) {
			printf("RESULT=ERROR TRY %ux%u: %s\n", only_w, only_h,
			       strerror(-ret));
			close(fd);
			return 2;
		}
		ret = g_fmt(fd, V4L2_SUBDEV_FORMAT_ACTIVE, pad, &cur);
		if (ret) {
			printf("RESULT=ERROR ACTIVE G_FMT: %s\n", strerror(-ret));
			close(fd);
			return 2;
		}
		changed = !same(&cur, &base);
		printf("TRY %ux%u -> %ux%u | ACTIVE %ux%u%s\n", only_w, only_h,
		       try_.width, try_.height, cur.width, cur.height,
		       changed ? " **CHANGED**" : " ok");
		printf("RESULT=%s\n", changed ? "FAIL" : "PASS");
		close(fd);
		return changed ? 1 : 0;
	}

	printf("=== TRY sweep (pad=%d, %d cases) ===\n", pad, n);
	for (i = 0; i < n; i++) {
		ret = t_fmt(fd, pad, cases[i][0], cases[i][1], &try_);
		if (ret) {
			printf("[%2d] try %ux%u -> TRY ERROR %s\n", i + 1,
			       cases[i][0], cases[i][1], strerror(-ret));
			continue;
		}
		ret = g_fmt(fd, V4L2_SUBDEV_FORMAT_ACTIVE, pad, &cur);
		if (ret) {
			printf("[%2d] try %ux%u -> ACTIVE G_FMT ERROR %s\n",
			       i + 1, cases[i][0], cases[i][1], strerror(-ret));
			changed++;
			continue;
		}
		if (!same(&cur, &base)) {
			printf("[%2d] try %4ux%-4u -> TRY %ux%u | ACTIVE %ux%u"
			       " **CHANGED** (baseline %ux%u)\n",
			       i + 1, cases[i][0], cases[i][1], try_.width,
			       try_.height, cur.width, cur.height, base.width,
			       base.height);
			changed++;
		} else {
			printf("[%2d] try %4ux%-4u -> TRY %ux%u | ACTIVE %ux%u ok\n",
			       i + 1, cases[i][0], cases[i][1], try_.width,
			       try_.height, cur.width, cur.height);
		}
	}

	if (g_fmt(fd, V4L2_SUBDEV_FORMAT_ACTIVE, pad, &cur) == 0)
		printf("=== after sweep (ACTIVE) ===\nACTIVE: %ux%u code=0x%04x%s\n",
		       cur.width, cur.height, cur.code,
		       same(&cur, &base) ? "" : "  <-- 已被 TRY 改变");

	close(fd);
	printf("RESULT=%s changed=%d/%d\n", changed ? "FAIL" : "PASS", changed,
	       n);
	return changed ? 1 : 0;
}
