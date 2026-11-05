/* Host-only V4L2 and stdio faults for focus_score and hdr_cfg. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/rk-camera-module.h>

static int fake_open(const char *, int, ...);
static int fake_close(int);
static int fake_ioctl(int, unsigned long, ...);
static void *fake_mmap(void *, size_t, int, int, int, off_t);
static int fake_munmap(void *, size_t);
static FILE *fake_fopen(const char *, const char *);
static size_t fake_fwrite(const void *, size_t, size_t, FILE *);
static int fake_fclose(FILE *);

#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define mmap fake_mmap
#define munmap fake_munmap
#define fopen fake_fopen
#define fwrite fake_fwrite
#define fclose fake_fclose
#define main focus_main
#include "../focus_score.c"
#undef main
#define main hdr_cfg_main
#include "../hdr_cfg.c"
#undef main
#undef open
#undef close
#undef ioctl
#undef mmap
#undef munmap
#undef fopen
#undef fwrite
#undef fclose

static int io_fault, hdr_fault;
static unsigned char frame[64 * 64];

static void check(int ok, const char *msg)
{
	if (!ok) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}
static int fake_open(const char *p, int flags, ...) { (void)p; (void)flags; return 100; }
static int fake_close(int fd) { (void)fd; if (hdr_fault == 6) { errno = EIO; return -1; } return 0; }
static int fake_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	(void)fd;
	va_start(ap, req); arg = va_arg(ap, void *); va_end(ap);
	if ((hdr_fault == 1 && req == RKMODULE_SET_HDR_CFG) ||
	    (hdr_fault == 2 && req == RKMODULE_GET_HDR_CFG) ||
	    (hdr_fault == 3 && req == VIDIOC_SUBDEV_S_FMT) ||
	    (hdr_fault == 4 && req == VIDIOC_SUBDEV_G_FMT)) {
		errno = EIO; return -1;
	}
	if (req == VIDIOC_S_FMT || req == VIDIOC_G_FMT) {
		struct v4l2_format *f = arg;
		f->fmt.pix_mp.width = f->fmt.pix_mp.height = 64;
		f->fmt.pix_mp.num_planes = 1;
		f->fmt.pix_mp.plane_fmt[0].sizeimage = sizeof(frame);
		f->fmt.pix_mp.plane_fmt[0].bytesperline = 64;
	} else if (req == VIDIOC_REQBUFS) {
		((struct v4l2_requestbuffers *)arg)->count = 1;
	} else if (req == VIDIOC_QUERYBUF) {
		((struct v4l2_buffer *)arg)->m.planes[0].length = sizeof(frame);
	} else if (req == VIDIOC_DQBUF) {
		((struct v4l2_buffer *)arg)->index = 0;
	} else if (req == RKMODULE_GET_HDR_CFG) {
		((struct rkmodule_hdr_cfg *)arg)->hdr_mode = hdr_fault == 5 ? HDR_X3 : HDR_X2;
	}
	return 0;
}
static void *fake_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{ (void)p; (void)n; (void)prot; (void)flags; (void)fd; (void)off; return frame; }
static int fake_munmap(void *p, size_t n) { (void)p; (void)n; return 0; }
static FILE *fake_fopen(const char *p, const char *mode)
{ (void)p; (void)mode; if (io_fault == 1) { errno = EACCES; return NULL; } return tmpfile(); }
static size_t fake_fwrite(const void *p, size_t s, size_t n, FILE *f)
{ if (io_fault == 2) { errno = ENOSPC; return 0; } return fwrite(p, s, n, f); }
static int fake_fclose(FILE *f)
{ int rc = fclose(f); if (io_fault == 3) { errno = EIO; return EOF; } return rc; }

int main(void)
{
	char *focus_argv[] = {"focus", "fake", "1", "best", "64", "64", NULL};
	char *hdr_argv[] = {"hdr", "fake", NULL};
	int i, x, y;
	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++) frame[y * 64 + x] = (uint8_t)(x * x + y * 3);
	for (i = 0; i <= 3; i++) {
		io_fault = i;
		check(focus_main(6, focus_argv) == (i ? 1 : 0), "focus output status");
	}
	io_fault = 0; memset(frame, 0, sizeof(frame));
	check(focus_main(6, focus_argv) != 0, "focus missing output status");
	for (i = 0; i <= 6; i++) {
		hdr_fault = i;
		check(hdr_cfg_main(2, hdr_argv) == (i ? 1 : 0), "HDR failure status");
	}
	puts("focus open/write/close and HDR ioctl/readback/close failures: PASS");
	return 0;
}
