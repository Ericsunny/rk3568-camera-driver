#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Observer only: RKISP21 metadata header, no frame data and no modifications. */
struct mapping { int dev; off_t offset; void *ptr; size_t length; };
struct buffer { int dev; unsigned index; off_t offset; };
static struct mapping maps[64];
static struct buffer bufs[64];
static int nmap, nbuf, logfd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int (*next_ioctl)(int, unsigned long, ...);
static void *(*next_mmap)(void *, size_t, int, int, int, off_t);
static int (*next_munmap)(void *, size_t);

__attribute__((constructor)) static void init(void) {
    const char *path = getenv("ISP_PARAM_TRACE_OUT");
    next_ioctl = dlsym(RTLD_NEXT, "ioctl");
    next_mmap = dlsym(RTLD_NEXT, "mmap");
    next_munmap = dlsym(RTLD_NEXT, "munmap");
    if (path) logfd = syscall(SYS_openat, AT_FDCWD, path, O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC, 0600);
}
static int target(int fd) {
    char key[48], name[128];
    ssize_t n;
    snprintf(key, sizeof(key), "/proc/self/fd/%d", fd);
    n = readlink(key, name, sizeof(name)-1);
    if (n < 0) return 0;
    name[n] = 0;
    if (!strcmp(name, "/dev/video13")) return 13;
    if (!strcmp(name, "/dev/video22")) return 22;
    return 0;
}
static void emit(const char *s) {
    if (logfd >= 0) (void)syscall(SYS_write, logfd, s, strlen(s));
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    void *p = next_mmap ? next_mmap(addr, len, prot, flags, fd, off)
                        : (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, off);
    int saved = errno, dev = target(fd);
    if (dev && p != MAP_FAILED) {
        char line[192];
        snprintf(line, sizeof(line), "MMAP dev=%d fd=%d off=%lld len=%zu\n", dev, fd, (long long)off, len);
        emit(line);
        pthread_mutex_lock(&lock);
        if (nmap < 64) maps[nmap++] = (struct mapping){dev, off, p, len};
        else emit("MAP_OVERFLOW\n");
        pthread_mutex_unlock(&lock);
    }
    errno = saved;
    return p;
}
int munmap(void *addr, size_t len) {
    int ret = next_munmap ? next_munmap(addr, len) : syscall(SYS_munmap, addr, len), saved = errno;
    if (!ret) {
        pthread_mutex_lock(&lock);
        for (int i=0; i<nmap; i++) if (maps[i].ptr == addr && maps[i].length == len) {
            maps[i] = maps[--nmap]; break;
        }
        pthread_mutex_unlock(&lock);
    }
    errno = saved;
    return ret;
}
int ioctl(int fd, unsigned long request, ...) {
    va_list ap; void *arg; int dev, result, saved;
    struct v4l2_buffer *b;
    uint64_t en_update=0, ens=0, cfg_update=0;
    uint32_t frame=0; int found=0; unsigned index=0; struct timespec ts;
    va_start(ap, request); arg=va_arg(ap, void *); va_end(ap);
    dev=target(fd); b=arg;
    if (dev && (uint32_t)request == (uint32_t)VIDIOC_QBUF && b) {
        char line[192];
        snprintf(line, sizeof(line), "SEEN_QBUF dev=%d fd=%d type=%u mem=%u idx=%u\n", dev, fd, b->type, b->memory, b->index);
        emit(line);
    }
    if (dev && (uint32_t)request == (uint32_t)VIDIOC_QBUF && b && b->type == V4L2_BUF_TYPE_META_OUTPUT && b->memory == V4L2_MEMORY_MMAP) {
        index=b->index;
        pthread_mutex_lock(&lock);
        for (int i=0; i<nbuf && !found; i++) if (bufs[i].dev==dev && bufs[i].index==index)
            for (int j=0; j<nmap; j++) if (maps[j].dev==dev && maps[j].offset==bufs[i].offset && maps[j].length>=28) {
                unsigned char *p=maps[j].ptr;
                memcpy(&en_update,p,8); memcpy(&ens,p+8,8); memcpy(&cfg_update,p+16,8); memcpy(&frame,p+24,4); found=1; break;
            }
        pthread_mutex_unlock(&lock);
    }
    result=next_ioctl ? next_ioctl(fd,request,arg) : syscall(SYS_ioctl,fd,request,arg); saved=errno;
    if (dev && (uint32_t)request==(uint32_t)VIDIOC_QUERYBUF && b && result==0 && b->type==V4L2_BUF_TYPE_META_OUTPUT && b->memory==V4L2_MEMORY_MMAP) {
        pthread_mutex_lock(&lock);
        int i; for(i=0;i<nbuf;i++) if(bufs[i].dev==dev && bufs[i].index==b->index) break;
        if(i<64) { if(i==nbuf) nbuf++; bufs[i]=(struct buffer){dev,b->index,b->m.offset}; }
        else emit("BUF_OVERFLOW\n");
        pthread_mutex_unlock(&lock);
    }
    if (dev && (uint32_t)request==(uint32_t)VIDIOC_QBUF && b && b->type==V4L2_BUF_TYPE_META_OUTPUT && b->memory==V4L2_MEMORY_MMAP) {
        char line[320]; clock_gettime(CLOCK_MONOTONIC,&ts);
        snprintf(line,sizeof(line),"t=%lld.%09ld dev=%d idx=%u mapped=%d frame=%" PRIu32 " en_update=%016" PRIx64 " ens=%016" PRIx64 " cfg_update=%016" PRIx64 " rc=%d errno=%d\n",(long long)ts.tv_sec,ts.tv_nsec,dev,index,found,frame,en_update,ens,cfg_update,result,result<0?saved:0);
        emit(line);
    }
    errno=saved; return result;
}
