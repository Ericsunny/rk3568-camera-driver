/* Linux linker wrappers for the complete mini3a executable. No devices used.
 * Build against the actual BSP UAPI: this fixture simulates responses, not ISP
 * operation or algorithm quality. See test_mini3a_host.py (BSP_UAPI).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef unsigned char u8; typedef signed char s8;
typedef unsigned short u16; typedef signed short s16;
typedef unsigned int u32; typedef signed int s32;
typedef unsigned long long u64; typedef signed long long s64;
#define BIT(x) (1U << (x))
#define BIT_ULL(x) (1ULL << (x))
#include <linux/rkisp21-config.h>
#include <linux/rkisp2-config.h>

enum { VIDEO=80, SENSOR=81, PARAMS=82 };
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static int queued[4], dq_calls, q_calls;
static void *maps[16];
static void *capture_maps[4];
static int capture_map_count;
static int scenario(const char *s) {
 const char *p=getenv("MINI3A_FAULT"); return p && !strcmp(p,s);
}
int __real_open(const char *,int,...);
int __real_close(int);
int __wrap_open(const char *p,int flags,...) {
 if(!strcmp(p,"/dev/fake-video")) return VIDEO;
 if(!strcmp(p,"/dev/fake-sensor")) return SENSOR;
 if(!strcmp(p,"/dev/fake-params")) return PARAMS;
 if(flags&O_CREAT) {
  va_list ap;va_start(ap,flags);int mode=va_arg(ap,int);va_end(ap);
  return __real_open(p,flags,mode);
 }
 return __real_open(p,flags);
}
int __wrap_close(int fd) {
 return fd==VIDEO||fd==SENSOR||fd==PARAMS ? 0 : __real_close(fd);
}
void *__real_mmap(void *,size_t,int,int,int,off_t);
int __real_munmap(void *,size_t);
void *__wrap_mmap(void *addr,size_t len,int prot,int flags,int fd,off_t off) {
 if(fd!=VIDEO && fd!=PARAMS) return __real_mmap(addr,len,prot,flags,fd,off);
 if(scenario("mmap")) {errno=ENOMEM;return MAP_FAILED;}
 void *p=calloc(1,len);
 if(!p) return MAP_FAILED;
 for(int i=0;i<16;i++) if(!maps[i]) {maps[i]=p;break;}
 if(fd==VIDEO) {
  capture_maps[capture_map_count++]=p;
  unsigned char *b=p;
  memset(b,scenario("bright")?250:110,16);
  memset(b+16,128,8);
 }
 return p;
}
int __wrap_munmap(void *p,size_t len) {
 for(int i=0;i<16;i++) if(maps[i]==p) {maps[i]=NULL;free(p);return 0;}
 return __real_munmap(p,len);
}
int __real_ioctl(int,unsigned long,...);
int __wrap_ioctl(int fd,unsigned long req,...) {
 va_list ap;va_start(ap,req);void *arg=va_arg(ap,void *);va_end(ap);
 if(fd==SENSOR) {
  if(req==VIDIOC_QUERYCTRL) {
   struct v4l2_queryctrl *q=arg;
   q->type=V4L2_CTRL_TYPE_INTEGER;q->step=1;q->minimum=q->id==V4L2_CID_EXPOSURE?4:0;
   q->maximum=q->id==V4L2_CID_EXPOSURE?2242:240;return 0;
  }
  if(req==VIDIOC_G_EXT_CTRLS) {
   struct v4l2_ext_controls *c=arg;
   c->controls[0].value=c->controls[0].id==V4L2_CID_EXPOSURE?101:0;return 0;
  }
  if(req==VIDIOC_S_EXT_CTRLS) return 0;
 }
 if(fd==PARAMS) {
  if(req==VIDIOC_G_FMT) ((struct v4l2_format*)arg)->fmt.meta.buffersize=sizeof(struct isp21_isp_params_cfg);
  if(req==VIDIOC_QUERYBUF) ((struct v4l2_buffer*)arg)->length=sizeof(struct isp21_isp_params_cfg);
  if(req==VIDIOC_QBUF && scenario("params")) {errno=EIO;return -1;}
  return 0;
 }
 if(fd==VIDEO) {
  if(req==VIDIOC_S_FMT) {
   struct v4l2_pix_format_mplane *p=&((struct v4l2_format*)arg)->fmt.pix_mp;
   p->width=4;p->height=2;p->num_planes=1;p->plane_fmt[0].bytesperline=8;
   p->plane_fmt[0].sizeimage=24;
   if(scenario("format")) p->pixelformat=V4L2_PIX_FMT_SBGGR10;
  }
  if(req==VIDIOC_QUERYBUF) {
   struct v4l2_buffer *b=arg;b->m.planes[0].length=24;
  }
  if(req==VIDIOC_QBUF) {
   pthread_mutex_lock(&mu);
   int fail=scenario("qbuf") && ++q_calls>4;
   if(!fail) queued[((struct v4l2_buffer*)arg)->index]=1;
   pthread_mutex_unlock(&mu);
   if(fail) {errno=EIO;return -1;}
  }
  if(req==VIDIOC_DQBUF) {
   pthread_mutex_lock(&mu);
   if(scenario("dqbuf") && dq_calls>=2) {pthread_mutex_unlock(&mu);errno=EIO;return -1;}
   for(int i=0;i<4;i++) if(queued[i]) {
    queued[i]=0;dq_calls++;
    if(scenario("drift") && dq_calls>20) memset((unsigned char*)capture_maps[i]+16,160,8);
    pthread_mutex_unlock(&mu);
    struct v4l2_buffer *b=arg;b->index=i;b->m.planes[0].bytesused=24;return 0;
   }
   pthread_mutex_unlock(&mu);errno=EAGAIN;return -1;
  }
  if(req==VIDIOC_STREAMOFF && scenario("streamoff")) {errno=EIO;return -1;}
  return 0;
 }
 return __real_ioctl(fd,req,arg);
}
int __real_poll(struct pollfd *,nfds_t,int);
int __wrap_poll(struct pollfd *p,nfds_t n,int ms) {
 if(n!=1 || p[0].fd!=VIDEO) return __real_poll(p,n,ms);
 usleep(1000);
 pthread_mutex_lock(&mu);
 int ready=queued[0]||queued[1]||queued[2]||queued[3];
 pthread_mutex_unlock(&mu);
 p[0].revents=ready?POLLIN:0;return ready;
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p) {
 return scenario("thread")?EAGAIN:__real_pthread_create(t,a,f,p);
}
int __real_fclose(FILE *);
int __wrap_fclose(FILE *f) {
 int rc=__real_fclose(f);return scenario("fclose")?EOF:rc;
}
