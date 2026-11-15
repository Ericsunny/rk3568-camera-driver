/* Board-only V4L2 lifecycle probe. No image access, encoding or inference. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

static double monotonic(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int call(int fd, unsigned long cmd, void *arg)
{
	int rc;
	do { rc = ioctl(fd, cmd, arg); } while (rc < 0 && errno == EINTR && !stop);
	return rc;
}

int main(int argc, char **argv)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct v4l2_requestbuffers req = { .count = 4,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct sigaction sa = { .sa_handler = on_signal };
	unsigned count = 0, previous = 0;
	int fd = -1, streaming = 0, rc = 1;
	unsigned i;

	if (argc != 2) {
		fprintf(stderr, "usage: %s VIDEO_DEVICE\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) { perror("open"); goto out; }
	fmt.fmt.pix_mp.width = 1280;
	fmt.fmt.pix_mp.height = 960;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (call(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); goto out; }
	if (fmt.fmt.pix_mp.width != 1280 || fmt.fmt.pix_mp.height != 960 ||
	    fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
	    fmt.fmt.pix_mp.num_planes != 1) {
		fprintf(stderr, "unexpected format\n");
		goto out;
	}
	if (call(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2 || req.count > 8) {
		perror("REQBUFS"); goto out;
	}
	for (i = 0; i < req.count; ++i) {
		struct v4l2_plane plane = {0};
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
			.index = i, .length = 1, .m.planes = &plane };
		if (call(fd, VIDIOC_QUERYBUF, &b) < 0 || call(fd, VIDIOC_QBUF, &b) < 0) {
			perror("QUERYBUF/QBUF"); goto out;
		}
	}
	printf("STREAMON_BEGIN %.9f\n", monotonic());
	if (call(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); goto out; }
	streaming = 1;
	printf("STREAMON_END %.9f\n", monotonic());
	while (!stop && count < 90) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct v4l2_plane plane = {0};
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
			.length = 1, .m.planes = &plane };
		int pr = poll(&pfd, 1, 2000);
		if (pr < 0 && errno == EINTR) continue;
		if (pr <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			fprintf(stderr, "poll failed/timeout revents=0x%x\n", pfd.revents);
			goto out;
		}
		if (call(fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN) continue;
			perror("DQBUF"); goto out;
		}
		printf("FRAME t=%.9f n=%u seq=%u flags=0x%x bytes=%u timestamp=%ld.%06ld\n",
		       monotonic(), count, b.sequence, b.flags, plane.bytesused,
		       (long)b.timestamp.tv_sec, (long)b.timestamp.tv_usec);
		if (b.index >= req.count || (b.flags & V4L2_BUF_FLAG_ERROR) ||
		    plane.bytesused < fmt.fmt.pix_mp.plane_fmt[0].bytesperline * 960U * 3 / 2 ||
		    (count && b.sequence != previous + 1)) {
			fprintf(stderr, "frame contract failed\n");
			goto out;
		}
		previous = b.sequence;
		++count;
		if (call(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); goto out; }
	}
	rc = stop ? 1 : 0;
out:
	if (streaming) {
		printf("STREAMOFF_BEGIN %.9f\n", monotonic());
		if (call(fd, VIDIOC_STREAMOFF, &type) < 0) { perror("STREAMOFF"); rc = 1; }
		printf("STREAMOFF_END %.9f\n", monotonic());
	}
	if (fd >= 0) {
		req.count = 0;
		if (call(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS release"); rc = 1; }
		if (close(fd) < 0) { perror("close"); rc = 1; }
	}
	printf("RESULT rc=%d frames=%u stopped=%d\n", rc, count, (int)stop);
	return rc;
}
