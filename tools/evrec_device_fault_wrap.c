/* Test-only linker wrappers: never link into the installed evrec binary.
 * Build evrec.c + rtsp_hub.c with --wrap=ioctl and --wrap=imresize_t.
 * See tools/evrec_device_fault_wrap.md for the audited call/ABI boundary. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <im2d.h>

enum fault_kind { FAULT_QBUF, FAULT_RGA };
static enum fault_kind fault;
static unsigned long target, seen;
static int fired, dequeued_fd = -1;
static dev_t camera_rdev;
static char camera_name[PATH_MAX];
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

extern int __real_ioctl(int fd, unsigned long request, ...);
extern IM_STATUS __real_imresize_t(const rga_buffer_t src, rga_buffer_t dst,
                                   double fx, double fy, int interpolation, int sync);

static int log_line(const char *s, size_t n)
{
	while (n) {
		ssize_t written = write(STDERR_FILENO, s, n);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) return -1;
		s += written; n -= (size_t)written;
	}
	return 0;
}

static void bad_config(const char *message)
{
	static const char prefix[] = "[evdf] CONFIG ERROR: ";
	if (log_line(prefix, sizeof(prefix) - 1) == 0 &&
	    log_line(message, strlen(message)) == 0)
		(void)log_line("\n", 1);
	_exit(125);
}

__attribute__((constructor)) static void init_fault(void)
{
	const char *mode = getenv("EVDF_MODE"), *at = getenv("EVDF_HIT");
	char *end;
	if (!mode) bad_config("missing EVDF_MODE");
	if (!strcmp(mode, "qbuf")) {
		const char *camera = getenv("EVDF_CAMERA");
		struct stat st;
		fault = FAULT_QBUF;
		if (!camera || camera[0] != '/' || !realpath(camera, camera_name) ||
		    stat(camera_name, &st) || !S_ISCHR(st.st_mode))
			bad_config("EVDF_CAMERA must name an existing character device");
		camera_rdev = st.st_rdev;
	} else if (!strcmp(mode, "rga")) {
		fault = FAULT_RGA;
	} else bad_config("unknown EVDF_MODE");
	if (!at || !*at || *at == '-' || *at == '+') bad_config("invalid EVDF_HIT");
	errno = 0;
	target = strtoul(at, &end, 10);
	if (errno || *end || !target) bad_config("EVDF_HIT must be a positive integer");
	{
		static const char msg[] = "[evdf] armed; test binary only\n";
		if (log_line(msg, sizeof(msg) - 1)) _exit(125);
	}
}

/* Caller owns state_lock. A missing or unwritable log invalidates the run. */
static int inject(enum fault_kind kind, const char *op, int fd)
{
	char line[PATH_MAX + 192];
	int n;
	if (fault != kind || fired) return 0;
	seen++;
	if (seen != target) return 0;
	fired = 1;
	n = snprintf(line, sizeof(line),
		     "[evdf] HIT operation=%s occurrence=%lu pid=%ld fd=%d device=%s "
		     "real=not-called reported=%d errno=%d\n", op, seen, (long)getpid(), fd,
		     kind == FAULT_QBUF ? camera_name : "n/a",
		     kind == FAULT_QBUF ? -1 : (int)IM_STATUS_FAILED,
		     kind == FAULT_QBUF ? EIO : 0);
	if (n <= 0 || (size_t)n >= sizeof(line) || log_line(line, (size_t)n)) _exit(125);
	return 1;
}

static int same_camera(int fd)
{
	struct stat st;
	int saved = errno;
	int same = fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == camera_rdev;
	errno = saved;
	return same;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int match, fail = 0, rc;
	/* Audited evrec.c and rtsp_hub.c pass a third argument at each call.
	 * GNU ld --wrap affects these linked objects, not calls inside shared libs. */
	va_start(ap, request); arg = va_arg(ap, void *); va_end(ap);
	match = fault == FAULT_QBUF && same_camera(fd);
	if (match && request == VIDIOC_QBUF) {
		pthread_mutex_lock(&state_lock);
		if (dequeued_fd == fd) {
			dequeued_fd = -1;
			fail = inject(FAULT_QBUF, "qbuf-after-dqbuf", fd);
		}
		pthread_mutex_unlock(&state_lock);
		if (fail) { errno = EIO; return -1; }
	}
	rc = __real_ioctl(fd, request, arg);
	if (match && request == VIDIOC_DQBUF && rc == 0) {
		pthread_mutex_lock(&state_lock);
		dequeued_fd = fd;
		pthread_mutex_unlock(&state_lock);
	} else if (match && (request == VIDIOC_STREAMOFF || request == VIDIOC_REQBUFS)) {
		pthread_mutex_lock(&state_lock);
		if (dequeued_fd == fd) dequeued_fd = -1;
		pthread_mutex_unlock(&state_lock);
	}
	return rc;
}

IM_STATUS __wrap_imresize_t(const rga_buffer_t src, rga_buffer_t dst,
                            double fx, double fy, int interpolation, int sync)
{
	int fail;
	pthread_mutex_lock(&state_lock);
	fail = inject(FAULT_RGA, "imresize", -1);
	pthread_mutex_unlock(&state_lock);
	if (fail) return IM_STATUS_FAILED;
	return __real_imresize_t(src, dst, fx, fy, interpolation, sync);
}
