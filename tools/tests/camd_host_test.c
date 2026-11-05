/* Full camd translation unit with a fake V4L2 device and injectable failures.
 * No camera, board, RGA, RKNN, or root filesystem writes are used. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

/* The original source places these CLI globals inside HAVE_RKNN although
 * the non-RKNN code also references them. Test-only declarations allow the
 * host harness to compile without vendor libraries; they do not fix that bug. */
static int g_exp = -1, g_gain = -1, g_no_obj_mul, g_rot90, g_wb;
static float g_obj_thresh;
static const char *g_ctrl_dev, *g_draw_file, *g_dump_rgb, *g_image;

static int fake_open(const char *, int, ...);
static int fake_close(int);
static int fake_ioctl(int, unsigned long, ...);
static void *fake_mmap(void *, size_t, int, int, int, off_t);
static int fake_munmap(void *, size_t);
static int fake_poll(struct pollfd *, nfds_t, int);
static void *fake_calloc(size_t, size_t);
static void fake_free(void *);
static int fake_pthread_create(pthread_t *, const pthread_attr_t *,
                               void *(*)(void *), void *);
static int fake_pthread_join(pthread_t, void **);
static int fake_nanosleep(const struct timespec *, struct timespec *);
static int fake_mkdir(const char *, mode_t);
static int fake_system(const char *);
static FILE *fake_fopen(const char *, const char *);

#define main camd_original_main
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define mmap fake_mmap
#define munmap fake_munmap
#define poll fake_poll
#define calloc fake_calloc
#define free fake_free
#define pthread_create fake_pthread_create
#define pthread_join fake_pthread_join
#define nanosleep fake_nanosleep
#define mkdir fake_mkdir
#define system fake_system
#define fopen fake_fopen
#include "../camd.c"
#undef main
#undef open
#undef close
#undef ioctl
#undef mmap
#undef munmap
#undef poll
#undef calloc
#undef free
#undef pthread_create
#undef pthread_join
#undef nanosleep
#undef mkdir
#undef system
#undef fopen

static int fail_create, create_calls, join_calls, fail_node, fail_map, fail_lat;
static int node_alloc_calls, map_calls, unmap_calls;
static int closed_device, closed_export, streamoff_calls;
static atomic_int sampler_active, sampler_overlap, sampler_max;
static unsigned char fake_buffer[8][128];
static void *lat_alloc;
static int lat_allocs, lat_frees;

static void require(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static int fake_open(const char *path, int flags, ...)
{
    (void)flags;
    if (strcmp(path, g_dev) == 0) return 100;
    errno = ENOENT;
    return -1;
}
static int fake_close(int fd)
{
    if (fd == 100) closed_device++;
    if (fd >= 200 && fd < 208) closed_export++;
    return 0;
}
static int fake_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    (void)fd;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (req == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        b->m.planes[0].length = sizeof(fake_buffer[0]);
        b->m.planes[0].m.mem_offset = b->index * sizeof(fake_buffer[0]);
    } else if (req == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *exp = arg;
        exp->fd = 200 + exp->index;
    } else if (req == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        b->index = 0;
        b->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    } else if (req == VIDIOC_STREAMOFF) {
        streamoff_calls++;
    }
    return 0;
}
static void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd;
    map_calls++;
    if (fail_map == map_calls) { errno = ENOMEM; return MAP_FAILED; }
    return fake_buffer[(size_t)off / sizeof(fake_buffer[0])];
}
static int fake_munmap(void *addr, size_t len)
{ (void)addr; (void)len; unmap_calls++; return 0; }
static int fake_poll(struct pollfd *p, nfds_t n, int timeout)
{ (void)p; (void)n; (void)timeout; return 1; }
static void *fake_calloc(size_t n, size_t size)
{
    void *p;
    if (n == 1 && size == sizeof(struct node)) {
        node_alloc_calls++;
        if (fail_node == node_alloc_calls) {
            errno = ENOMEM;
            return NULL;
        }
    }
    if (fail_lat && n == 8 && size == sizeof(uint32_t)) {
        errno = ENOMEM;
        return NULL;
    }
    p = calloc(n, size);
    if (n == 8 && size == sizeof(uint32_t) && p) {
        lat_alloc = p;
        lat_allocs++;
    }
    return p;
}
static void fake_free(void *p)
{
    if (p && p == lat_alloc) { lat_frees++; lat_alloc = NULL; }
    free(p);
}
static void *sampler_proxy(void *unused)
{
    int active = atomic_fetch_add(&sampler_active, 1) + 1;
    int old = atomic_load(&sampler_max);
    (void)unused;
    if (active > 1) atomic_store(&sampler_overlap, 1);
    while (old < active && !atomic_compare_exchange_weak(&sampler_max, &old, active)) {}
    sampler_thread(NULL);
    /* A delayed exit makes an unjoined sampler overlap a subsequent session. */
    usleep(2000);
    atomic_fetch_sub(&sampler_active, 1);
    return NULL;
}
static int fake_pthread_create(pthread_t *t, const pthread_attr_t *a,
                               void *(*fn)(void *), void *arg)
{
    create_calls++;
    if (create_calls == fail_create) {
        if (fail_create >= 3) {
            int i, ready = 0;
            /* Ensure capture has published one reference to each queue before
             * injecting a consumer-creation failure. */
            for (i = 0; i < 2000; i++) {
                pthread_mutex_lock(&q[Q_RT].m);
                pthread_mutex_lock(&q[Q_EV].m);
                ready = q[Q_RT].pushed == 1 && q[Q_EV].pushed == 1;
                pthread_mutex_unlock(&q[Q_EV].m);
                pthread_mutex_unlock(&q[Q_RT].m);
                if (ready) break;
                usleep(1000);
            }
            require(ready, "capture published frame before injected failure");
        }
        return EAGAIN;
    }
    return pthread_create(t, a, fn == sampler_thread ? sampler_proxy : fn, arg);
}
static int fake_pthread_join(pthread_t t, void **result)
{
    int rc = pthread_join(t, result);
    if (rc == 0) join_calls++;
    return rc;
}
static int fake_nanosleep(const struct timespec *req, struct timespec *rem)
{
    const struct timespec short_delay = {0, 1000000};
    (void)req;
    return nanosleep(&short_delay, rem);
}
static int fake_mkdir(const char *path, mode_t mode)
{ (void)path; (void)mode; errno = EACCES; return -1; }
static int fake_system(const char *command) { (void)command; return 0; }
static FILE *fake_fopen(const char *path, const char *mode)
{
    if (strncmp(path, "/root/camd_diag_", 16) == 0) { errno = EACCES; return NULL; }
    return fopen(path, mode);
}

static void setup_session(void)
{
    fail_create = create_calls = join_calls = 0;
    fail_node = fail_map = 0;
    node_alloc_calls = map_calls = unmap_calls = 0;
    closed_device = closed_export = streamoff_calls = 0;
    cap.fd = -1; cap.nbufs = 1; cap.w = 4; cap.h = 4;
    cap.fourcc = V4L2_PIX_FMT_NV12;
    g_frames_limit = 1; g_ev_fps = 0; g_copy = g_rga = 0;
    g_exp = g_gain = -1; g_fault = 0; g_fault_once = 0;
}

int main(void)
{
    int i, rc;
    lat_t test_lat = {0};

    for (i = 1; i <= 4; i++) {
        setup_session(); fail_create = i;
        rc = session();
        require(rc != 0, "pthread_create failure returns error");
        require(join_calls == i - 1, "only successfully created threads joined");
        require(!atomic_load(&sampler_active), "sampler joined on create failure");
        require(closed_device == 1 && closed_export == 1 && streamoff_calls == 1,
                "create failure closes resources");
        if (i >= 3)
            require(atomic_load(&g_frames_done) == 1 && q[Q_RT].n == 0 &&
                    q[Q_EV].n == 0, "queued frame references returned on failure");
        printf("thread failure %d: rc=%d joined=%d PASS\n", i, rc, join_calls);
    }

    setup_session(); fail_node = 1;
    rc = session();
    require(rc != 0 && closed_export == 1 && closed_device == 1,
            "node allocation failure closes exported and device fds");
    require(create_calls == 0, "no worker started after allocation failure");
    puts("node allocation failure PASS");

    setup_session(); cap.nbufs = 2; fail_node = 2;
    rc = session();
    require(rc != 0 && closed_export == 2 && closed_device == 1 && unmap_calls == 1,
            "later allocation failure cleans already initialized resources");
    puts("later node allocation failure cleanup PASS");

    setup_session(); fail_map = 1; g_copy = 1;
    rc = session();
    require(rc != 0 && closed_export == 1 && closed_device == 1,
            "required mmap failure closes resources");
    require(create_calls == 0, "no worker started after required mmap failure");
    puts("required mapping failure PASS");

    setup_session(); cap.nbufs = 2; fail_map = 2; g_copy = 1;
    rc = session();
    require(rc != 0 && closed_export == 2 && closed_device == 1 && unmap_calls == 1,
            "later mapping failure cleans already initialized resources");
    puts("later mapping failure cleanup PASS");

    setup_session(); fail_map = 1; g_copy = 0;
    rc = session();
    require(rc == 0 && atomic_load(&g_frames) == 1,
            "optional mapping failure retains non-copy behavior");
    require(atomic_load(&g_frames_done) == 1 && q[Q_RT].pushed == 1 &&
            q[Q_RT].popped == 1 && q[Q_EV].pushed == 1 && q[Q_EV].popped == 1,
            "normal frame return and queue counts");
    require(join_calls == 4 && !atomic_load(&sampler_active), "normal joins all workers");
    puts("optional mapping failure and normal session PASS");

    for (i = 0; i < 100; i++) lat_init(&test_lat, 8);
    lat_init(&test_lat, 0);
    require(lat_allocs == 100 && lat_frees == 100 && !test_lat.ring,
            "latency rings freed on repeated initialization");
    printf("latency reinitializations alloc=%d free=%d PASS\n", lat_allocs, lat_frees);
    fail_lat = 1;
    lat_init(&test_lat, 8);
    lat_add(&test_lat, 123);
    require(!test_lat.ring && test_lat.cap == 0 && lat_pct(&test_lat, 0.99) == -1,
            "latency ring allocation failure returns unavailable percentile");
    fail_lat = 0;
    puts("latency ring allocation failure PASS");

    for (i = 0; i < 30; i++) {
        setup_session(); g_fault = 2; g_fault_once = 1;
        rc = session();
        require(rc == 2 && !atomic_load(&sampler_active), "fault session sampler stopped");
        setup_session();
        rc = session();
        require(rc == 0 && !atomic_load(&sampler_active), "recovery session sampler stopped");
    }
    require(!atomic_load(&sampler_overlap) && atomic_load(&sampler_max) == 1,
            "samplers never overlap across fault/recovery sessions");
    printf("30 fault/recovery pairs: max concurrent samplers=%d PASS\n",
           atomic_load(&sampler_max));
    return 0;
}
