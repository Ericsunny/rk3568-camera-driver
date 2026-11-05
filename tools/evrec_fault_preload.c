/*
 * Test-only, process-local stdio fault injector for docs/41 A1.
 * Never preload into the installed service. See tools/evrec_fault_preload.md.
 *
 * Deliberately interposes only APIs currently used by evrec.c. It is not a
 * security boundary: use a new, private directory with no untrusted writers.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum fault_mode { OPEN_JSON, WRITE_JSON, CLOSE_JSON, WRITE_VIDEO, RENAME_VIDEO };
enum stream_kind { JSON_STREAM, VIDEO_STREAM };
struct tracked_stream { FILE *stream; enum stream_kind kind; char path[PATH_MAX]; };
static struct tracked_stream tracked[64];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char target_dir[PATH_MAX];
static char target_prefix[NAME_MAX + 1];
static enum fault_mode mode;
static unsigned long target_hit, seen;
static int fired;
static FILE *(*real_fopen)(const char *, const char *);
static FILE *(*real_fopen64)(const char *, const char *);
static int (*real_vfprintf)(FILE *, const char *, va_list);
static int (*real_vfprintf_chk)(FILE *, int, const char *, va_list);
static size_t (*real_fwrite)(const void *, size_t, size_t, FILE *);
static int (*real_fclose)(FILE *);
static int (*real_rename)(const char *, const char *);

static int write_log(const char *text, size_t length)
{
	while (length) {
		ssize_t n = write(STDERR_FILENO, text, length);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		text += n;
		length -= (size_t)n;
	}
	return 0;
}

static void fatal(const char *message)
{
	static const char prefix[] = "[evfi] CONFIG ERROR: ";
	if (write_log(prefix, sizeof(prefix) - 1) == 0 &&
	    write_log(message, strlen(message)) == 0)
		(void)write_log("\n", 1);
	_exit(125);
}

static void *symbol(const char *name)
{
	void *p = dlsym(RTLD_NEXT, name);
	if (!p) fatal(name);
	return p;
}

__attribute__((constructor)) static void init_fault(void)
{
	const char *dir = getenv("EVFI_DIR"), *prefix = getenv("EVFI_PREFIX");
	const char *which = getenv("EVFI_MODE"), *hit = getenv("EVFI_HIT");
	char *end;
	struct stat st;

	real_fopen = symbol("fopen");
	real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
	if (!real_fopen64) real_fopen64 = real_fopen;
	real_vfprintf = symbol("vfprintf");
	real_vfprintf_chk = dlsym(RTLD_NEXT, "__vfprintf_chk");
	real_fwrite = symbol("fwrite");
	real_fclose = symbol("fclose");
	real_rename = symbol("rename");
	if (!dir || dir[0] != '/' || !realpath(dir, target_dir) ||
	    stat(target_dir, &st) || !S_ISDIR(st.st_mode) ||
	    !strcmp(target_dir, "/")) fatal("EVFI_DIR must name an existing non-root directory");
	if (!prefix || !*prefix || strlen(prefix) > NAME_MAX || strchr(prefix, '/') ||
	    !strcmp(prefix, ".") || !strcmp(prefix, "..")) fatal("invalid EVFI_PREFIX");
	strcpy(target_prefix, prefix);
	if (!which) fatal("missing EVFI_MODE");
	if (!strcmp(which, "json-open")) mode = OPEN_JSON;
	else if (!strcmp(which, "json-write")) mode = WRITE_JSON;
	else if (!strcmp(which, "json-close")) mode = CLOSE_JSON;
	else if (!strcmp(which, "video-write")) mode = WRITE_VIDEO;
	else if (!strcmp(which, "video-rename")) mode = RENAME_VIDEO;
	else fatal("unknown EVFI_MODE");
	if (!hit || !*hit || *hit == '-' || *hit == '+') fatal("invalid EVFI_HIT");
	errno = 0;
	target_hit = strtoul(hit, &end, 10);
	if (errno || *end || !target_hit) fatal("EVFI_HIT must be a positive integer");
	{
		static const char msg[] = "[evfi] armed; target paths must use canonical EVFI_DIR\n";
		if (write_log(msg, sizeof(msg) - 1) != 0) _exit(125);
	}
}

/* Only direct children with the explicit prefix/suffix; reject existing symlinks.
 * No realpath on the file itself: JSON may not yet exist. */
static int matches(const char *path, const char *suffix)
{
	size_t d = strlen(target_dir), p = strlen(target_prefix), s = strlen(suffix), n;
	const char *name;
	struct stat st;
	if (!path || strncmp(path, target_dir, d) || path[d] != '/') return 0;
	name = path + d + 1;
	n = strlen(name);
	if (strchr(name, '/') || n < p + s || strncmp(name, target_prefix, p) ||
	    strcmp(name + n - s, suffix)) return 0;
	if (lstat(path, &st) == 0 && !S_ISREG(st.st_mode)) return 0;
	return 1;
}

/* Caller holds lock. One eligible call per process, with an auditable hit. */
static int inject(enum fault_mode candidate, const char *operation, const char *path)
{
	char msg[PATH_MAX + 160];
	int n;
	if (candidate != mode || fired) return 0;
	seen++;
	if (seen != target_hit) return 0;
	fired = 1;
	n = snprintf(msg, sizeof(msg), "[evfi] HIT operation=%s occurrence=%lu pid=%ld path=%s\n",
		     operation, seen, (long)getpid(), path);
	if (n <= 0 || (size_t)n >= sizeof(msg) || write_log(msg, (size_t)n) != 0)
		_exit(125); /* no silent fault if the evidence channel is unavailable */
	return 1;
}

/* real_result is -2 when the real operation was intentionally not called. */
static void log_result(const char *operation, long real_result, long reported,
		       int error_code)
{
	char msg[200];
	int n = snprintf(msg, sizeof(msg),
			 "[evfi] RESULT operation=%s real=%ld reported=%ld errno=%d\n",
			 operation, real_result, reported, error_code);
	if (n <= 0 || (size_t)n >= sizeof(msg) || write_log(msg, (size_t)n) != 0)
		_exit(125);
}

static void track(FILE *f, enum stream_kind kind, const char *path)
{
	size_t i;
	if (!f) return;
	for (i = 0; i < sizeof(tracked) / sizeof(tracked[0]); i++) {
		if (!tracked[i].stream) {
			if (strlen(path) >= sizeof(tracked[i].path)) fatal("target path too long");
			tracked[i].stream = f; tracked[i].kind = kind;
			strcpy(tracked[i].path, path);
			return;
		}
	}
	fatal("too many tracked streams; no result is valid");
}

static const char *stream_path(FILE *f, enum stream_kind kind)
{
	size_t i;
	for (i = 0; i < sizeof(tracked) / sizeof(tracked[0]); i++)
		if (tracked[i].stream == f && tracked[i].kind == kind) return tracked[i].path;
	return NULL;
}

static void untrack(FILE *f)
{
	size_t i;
	for (i = 0; i < sizeof(tracked) / sizeof(tracked[0]); i++)
		if (tracked[i].stream == f) { tracked[i].stream = NULL; return; }
}

static FILE *open_common(const char *path, const char *flags,
			 FILE *(*open_fn)(const char *, const char *))
{
	FILE *f;
	int json, video, fail = 0;
	if (!flags || (flags[0] != 'w' && flags[0] != 'a')) return open_fn(path, flags);
	pthread_mutex_lock(&lock);
	json = matches(path, ".json");
	video = matches(path, ".part");
	if (json) fail = inject(OPEN_JSON, "json-open", path);
	if (fail) {
		pthread_mutex_unlock(&lock);
		log_result("json-open", -2, -1, EACCES);
		errno = EACCES;
		return NULL;
	}
	f = open_fn(path, flags);
	if (json) track(f, JSON_STREAM, path);
	if (video) track(f, VIDEO_STREAM, path);
	pthread_mutex_unlock(&lock);
	return f;
}

FILE *fopen(const char *path, const char *flags) { return open_common(path, flags, real_fopen); }
FILE *fopen64(const char *path, const char *flags) { return open_common(path, flags, real_fopen64); }

static int print_common(FILE *f, int fortify, const char *fmt, va_list ap)
{
	int fail;
	const char *path;
	pthread_mutex_lock(&lock);
	path = stream_path(f, JSON_STREAM);
	fail = path && inject(WRITE_JSON, "json-write", path);
	pthread_mutex_unlock(&lock);
	if (fail) {
		size_t partial;
		/* fprintf reports an error after an actual partial metadata write. */
		partial = real_fwrite("{", 1, 1, f);
		log_result("json-write", (long)partial, -1, ENOSPC);
		errno = ENOSPC;
		return -1;
	}
	if (fortify >= 0 && real_vfprintf_chk) return real_vfprintf_chk(f, fortify, fmt, ap);
	return real_vfprintf(f, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...)
{
	int rc; va_list ap;
	va_start(ap, fmt); rc = print_common(f, -1, fmt, ap); va_end(ap);
	return rc;
}

int __fprintf_chk(FILE *f, int flag, const char *fmt, ...)
{
	int rc; va_list ap;
	va_start(ap, fmt); rc = print_common(f, flag, fmt, ap); va_end(ap);
	return rc;
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f)
{
	int fail;
	const char *path;
	pthread_mutex_lock(&lock);
	path = stream_path(f, VIDEO_STREAM);
	fail = size && n && path && inject(WRITE_VIDEO, "video-write", path);
	pthread_mutex_unlock(&lock);
	if (fail) {
		size_t rc = real_fwrite(ptr, size, n - 1, f);
		log_result("video-write", (long)rc, (long)rc, ENOSPC);
		errno = ENOSPC;
		return rc;
	}
	return real_fwrite(ptr, size, n, f);
}

int fclose(FILE *f)
{
	int fail, rc;
	const char *path;
	pthread_mutex_lock(&lock);
	path = stream_path(f, JSON_STREAM);
	fail = path && inject(CLOSE_JSON, "json-close", path);
	untrack(f);
	pthread_mutex_unlock(&lock);
	rc = real_fclose(f);
	if (fail) { log_result("json-close", rc, EOF, EIO); errno = EIO; return EOF; }
	return rc;
}

int rename(const char *oldpath, const char *newpath)
{
	int fail;
	pthread_mutex_lock(&lock);
	fail = matches(oldpath, ".part") && matches(newpath, ".h264") &&
	       inject(RENAME_VIDEO, "video-rename", oldpath);
	pthread_mutex_unlock(&lock);
	if (fail) { log_result("video-rename", -2, -1, EIO); errno = EIO; return -1; }
	return real_rename(oldpath, newpath);
}
