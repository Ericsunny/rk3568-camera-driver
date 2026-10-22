/*
 * streamoff_test.c —— 验证「停流失败」是否能一路返回给用户态
 *
 * 用法: ./streamoff_test [/dev/video0] [w] [h]
 *
 * 流程: REQBUFS(4) → QBUF → STREAMON → DQBUF 2 帧 → STREAMOFF
 * 打印每一步的 ioctl 返回值/errno,重点看 STREAMOFF。
 *
 * 配合驱动故障注入:
 *   echo 5 > /sys/module/ov8858_sm/parameters/inject_stop_err   # 让 stop_stream 返回 -EIO
 * 预期:
 *   - 驱动侧 dmesg: "stop_stream failed: -5 (sensor 可能仍在出流)"
 *   - 用户态: STREAMOFF 是否返回 -1/errno=5 取决于上层(rkcif)有没有把
 *     v4l2_subdev_call(s_stream,0) 的返回值传上去 —— 这正是本实验要区分的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define NBUFS 4

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/video0";
	unsigned int w = (argc > 2) ? atoi(argv[2]) : 1632;
	unsigned int h = (argc > 3) ? atoi(argv[3]) : 1224;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
	struct v4l2_format f;
	int fd, i, ret, err;

	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("RESULT=ERROR open %s: %s\n", dev, strerror(errno));
		return 2;
	}

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SBGGR10;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	ret = ioctl(fd, VIDIOC_S_FMT, &f);
	printf("S_FMT: ret=%d errno=%d (%s) -> %ux%u\n", ret,
	       ret ? errno : 0, ret ? strerror(errno) : "ok",
	       f.fmt.pix_mp.width, f.fmt.pix_mp.height);

	memset(&rb, 0, sizeof(rb));
	rb.count = NBUFS;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(fd, VIDIOC_REQBUFS, &rb);
	printf("REQBUFS(4): ret=%d errno=%d (%s) count=%u\n", ret,
	       ret ? errno : 0, ret ? strerror(errno) : "ok", rb.count);

	for (i = 0; i < NBUFS; i++) {
		struct v4l2_plane planes[1];

		memset(&b, 0, sizeof(b));
		memset(planes, 0, sizeof(planes));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.m.planes = planes;
		b.length = 1;
		if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
			printf("QUERYBUF(%d) failed: %s\n", i, strerror(errno));
			close(fd);
			return 2;
		}
		if (ioctl(fd, VIDIOC_QBUF, &b) < 0)
			printf("QBUF(%d) failed: %s\n", i, strerror(errno));
	}

	{
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		ret = ioctl(fd, VIDIOC_STREAMON, &t);
		printf("STREAMON: ret=%d errno=%d (%s)\n", ret,
		       ret ? errno : 0, ret ? strerror(errno) : "ok");
		if (ret < 0) {
			close(fd);
			return 2;
		}
	}

	/* 取 2 帧,确认真的在出流 */
	for (i = 0; i < 2; i++) {
		struct v4l2_plane planes[1];
		int tries;

		for (tries = 0; tries < 100; tries++) {
			memset(&b, 0, sizeof(b));
			memset(planes, 0, sizeof(planes));
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.m.planes = planes;
			b.length = 1;
			if (ioctl(fd, VIDIOC_DQBUF, &b) == 0)
				break;
			if (errno != EAGAIN) {
				printf("DQBUF(%d) failed: %s\n", i,
				       strerror(errno));
				break;
			}
			usleep(20000);
		}
		if (tries == 100) {
			printf("DQBUF(%d): 超时,没拿到帧\n", i);
			break;
		}
		printf("DQBUF(%d): bytesused=%u index=%u\n", i,
		       planes[0].bytesused, b.index);
		if (ioctl(fd, VIDIOC_QBUF, &b) < 0)
			printf("QBUF back failed: %s\n", strerror(errno));
	}

	{
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		errno = 0;
		ret = ioctl(fd, VIDIOC_STREAMOFF, &t);
		err = errno;
		printf("STREAMOFF: ret=%d errno=%d (%s)\n", ret, err,
		       err ? strerror(err) : "ok");
		printf("RESULT=%s\n", (ret == 0) ? "OK_NO_ERROR" :
		       "ERROR_PROPAGATED");
	}

	close(fd);
	return 0;
}
