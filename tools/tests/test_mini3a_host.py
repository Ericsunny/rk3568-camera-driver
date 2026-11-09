#!/usr/bin/env python3
"""Linux host tests of actual mini3a source functions, using fake device I/O.

No camera, vendor libraries or ISP hardware are used. The tiny params structure
below is an I/O test double, not an ABI definition; build the full program with
the target BSP headers separately before a board test.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import time

SOURCE = (Path(__file__).resolve().parents[2] / 'mini3a/mini3a.c').read_text(encoding='utf-8')
ROOT = Path(__file__).resolve().parents[2]


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index('{', start)
    depth = 0
    for pos in range(opening, len(SOURCE)):
        depth += (SOURCE[pos] == '{') - (SOURCE[pos] == '}')
        if not depth:
            return SOURCE[start:pos + 1]
    raise ValueError(signature)


PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <linux/videodev2.h>
#define AE_Y_LO 90
#define AE_Y_HI 130
#define AWB_UV_TOL 24
#define ISP2X_MODULE_AWB_GAIN 1
static int exp_min=4, exp_max=2242, gain_min=0, gain_max=240;
static int cur_exp=101, cur_gain, ctrl_failure, ctrl_calls;
static int ae_apply_ctrl(void) { ctrl_calls++; return ctrl_failure ? -1 : 0; }
static int cur_gain_r=0x100, cur_gain_b=0x100;
static char *params_dev="fake-params", *output_path="frame.nv12";
static int frame_w=4, frame_h=2;
static unsigned frame_stride=8;
#define RING_SLOTS 4
static int vfd=8, nbufs=4, ring[RING_SLOTS], ring_head, ring_tail;
static atomic_int capture_done, capture_failed, dq_calls;
static volatile sig_atomic_t stop_signal;
static pthread_mutex_t ring_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_cv=PTHREAD_COND_INITIALIZER;
static int capture_io_failure;
static int fake_poll(struct pollfd *p,nfds_t n,int timeout) {
 (void)n;(void)timeout;p->revents=POLLIN;return 1;
}

/* Only the members accessed by the production AWB function: no ABI claim. */
struct gains {
 unsigned gain0_red,gain0_blue,gain1_red,gain1_blue,gain2_red,gain2_blue;
 unsigned gain0_green_r,gain0_green_b,gain1_green_r,gain1_green_b;
 unsigned gain2_green_r,gain2_green_b;
};
struct isp21_isp_params_cfg {
 unsigned module_en_update,module_ens,module_cfg_update;
 struct { struct gains awb_gain_cfg; } others;
};
static struct isp21_isp_params_cfg storage;
static unsigned long fail_request;
static int wrong_abi, close_calls, unmap_calls, fail_close, fail_streamoff;
static int mmap_failure, short_map;
static size_t expected_unmap;
static int fake_open(const char *path, int flags) {
 (void)flags; assert(!strcmp(path,params_dev)); return 7;
}
static int fake_close(int fd) { assert(fd==7); close_calls++; return 0; }
static void *fake_mmap(void *p,size_t n,int prot,int flags,int fd,off_t off) {
 (void)p;(void)prot;(void)flags;(void)fd;(void)off;
 expected_unmap=n; return mmap_failure ? MAP_FAILED : &storage;
}
static int fake_munmap(void *p,size_t n) {
 assert(p==&storage); assert(n==expected_unmap && n>0); unmap_calls++; return 0;
}
static int fake_usleep(useconds_t n) { (void)n; return 0; }
static int xioctl(int fd,unsigned long req,void *arg) {
 if(fd==vfd) {
  assert(req==VIDIOC_DQBUF);
  if(capture_io_failure) {errno=EIO;return -1;}
  struct v4l2_buffer *b=arg;
  b->index=atomic_fetch_add(&dq_calls,1)%4;
  b->m.planes[0].bytesused=frame_stride*frame_h*3/2;
  return 0;
 }
 assert(fd==7);
 if (req==fail_request || (fail_streamoff && req==VIDIOC_STREAMOFF)) {errno=EIO;return -1;}
 if (req==VIDIOC_G_FMT) ((struct v4l2_format*)arg)->fmt.meta.buffersize=sizeof(storage)+wrong_abi;
 if (req==VIDIOC_QUERYBUF) ((struct v4l2_buffer*)arg)->length=sizeof(storage)+(short_map ? -1 : 64);
 return 0;
}
static int checked_fclose(FILE *fp) {
 int rc=fclose(fp); return fail_close==1 ? EOF : rc;
}
static size_t checked_fwrite(const void *p,size_t s,size_t n,FILE *f) {
 if (fail_close==2) return n ? n-1 : 0;
 return fwrite(p,s,n,f);
}
'''

TESTS = r'''
static void reset_io(void) {
 fail_request=0;wrong_abi=close_calls=unmap_calls=0;
 mmap_failure=short_map=fail_streamoff=0;
}
static int wait_for_full(int calls) {
 for(int i=0;i<1000;i++) {
  int full;
  pthread_mutex_lock(&ring_mu);
  full=((ring_head+1)%RING_SLOTS==ring_tail);
  pthread_mutex_unlock(&ring_mu);
  if(full && dq_calls>=calls) return 1;
  usleep(1000);
 }
 return 0;
}
int main(void) {
 /* NV12 padded rows: padding must not contribute to statistics. */
 uint8_t image[24]={10,20,30,40,255,255,255,255,
                    50,60,70,80,255,255,255,255,
                    100,150,101,151,255,255,255,255};
 int y,u,v;
 frame_stats(image,4,2,8,&y,&u,&v);
 assert(y==45 && u==100 && v==150);
 /* A 4096-wide 4K frame exceeds INT_MAX in the old Y sum cast. */
 size_t size=(size_t)4096*2160*3/2;
 uint8_t *large=malloc(size);assert(large);memset(large,255,size);
 frame_stats(large,4096,2160,4096,&y,&u,&v);
 assert(y==255 && u==255 && v==255);free(large);

 /* Failed sensor controls must not commit the proposed software state. */
 cur_exp=101;cur_gain=0;ctrl_failure=1;
 assert(ae_step(10)==-1 && cur_exp==101 && cur_gain==0);
 ctrl_failure=0;assert(ae_step(10)==1 && cur_exp==202);
 int calls=ctrl_calls;assert(ae_step(110)==0 && ctrl_calls==calls);

 /* Actual params submission function, with fake syscalls at its boundary. */
 unsigned long failures[]={VIDIOC_G_FMT,VIDIOC_REQBUFS,VIDIOC_QUERYBUF,
                           VIDIOC_QBUF,VIDIOC_STREAMON,VIDIOC_STREAMOFF};
 for(size_t i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
  reset_io();fail_request=failures[i];
  assert(awb_apply_params(300,310)==-1 && close_calls==1);
  assert(unmap_calls==(i>=3));
 }
 reset_io();wrong_abi=1;assert(awb_apply_params(300,310)==-1 && unmap_calls==0);
 reset_io();short_map=1;assert(awb_apply_params(300,310)==-1 && unmap_calls==0);
 reset_io();mmap_failure=1;assert(awb_apply_params(300,310)==-1 && unmap_calls==0);
 reset_io();assert(awb_apply_params(300,310)==0 && unmap_calls==1);
 assert(storage.others.awb_gain_cfg.gain0_red==300);
 reset_io();fail_request=VIDIOC_QBUF;
 cur_gain_r=cur_gain_b=256;
 assert(awb_step(100,100)==-1 && cur_gain_r==256 && cur_gain_b==256);
 reset_io();assert(awb_step(100,100)==1 && cur_gain_r>256 && cur_gain_b>256);

 /* Saved frame omits padding; exclusive create preserves existing files. */
 assert(save_frame(image)==0);
 FILE *f=fopen(output_path,"rb");assert(f);
 uint8_t saved[24];assert(fread(saved,1,sizeof(saved),f)==12);fclose(f);
 uint8_t expected[]={10,20,30,40,50,60,70,80,100,150,101,151};
 assert(!memcmp(saved,expected,sizeof(expected)));
 assert(save_frame(image)==-1);
 output_path="missing-directory/frame.nv12";assert(save_frame(image)==-1);
 output_path="close-failed.nv12";fail_close=1;assert(save_frame(image)==-1);
 output_path="short-write.nv12";fail_close=2;assert(save_frame(image)==-1);

 /* Exercise the real producer, dequeue helper and stop while queue is full. */
 pthread_t tid;
 assert(pthread_create(&tid,NULL,capture_thread,NULL)==0);
 assert(wait_for_full(4));
 assert(ring_take()==0); /* removing an index must wake the blocked producer */
 assert(wait_for_full(5));
 capture_request_stop();assert(pthread_join(tid,NULL)==0);
 assert(capture_done && !capture_failed);
 capture_done=0;capture_failed=0;capture_io_failure=1;
 ring_head=ring_tail=0;
 assert(pthread_create(&tid,NULL,capture_thread,NULL)==0);
 assert(pthread_join(tid,NULL)==0);
 assert(capture_done && capture_failed); /* a failed producer marks completion */
 puts("PASS mini3a: statistics, control/params/output failures, queue wakeup and producer exit");
 return 0;
}
'''


def main():
    code = PRELUDE + function('static void frame_stats(') + function('static int ae_step(')
    code += '\n#define open fake_open\n#define close fake_close\n#define mmap fake_mmap\n'
    code += '#define munmap fake_munmap\n#define usleep fake_usleep\n'
    code += function('static int awb_apply_params(') + function('static int awb_step(')
    code += '\n#undef open\n#undef close\n#undef mmap\n#undef munmap\n#undef usleep\n'
    code += '#define fclose checked_fclose\n#define fwrite checked_fwrite\n'
    code += function('static int save_frame(')
    code += '\n#undef fclose\n#undef fwrite\n' + TESTS
    producer = '\n#define poll fake_poll\n' + function('static void *capture_thread(')
    producer += '\n#undef poll\n' + function('static void capture_request_stop(')
    producer += function('static int ring_take(')
    code = code.replace(TESTS, producer + TESTS)
    with tempfile.TemporaryDirectory(prefix='mini3a-host-') as path:
        work = Path(path)
        (work/'test.c').write_text(code, encoding='utf-8')
        command = [os.environ.get('CC','cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-O1', '-g', '-pthread', 'test.c', '-o', 'test']
        if os.environ.get('MINI3A_SANITIZE') == '1':
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, cwd=work, check=True, timeout=60)
        subprocess.run([str(work/'test')], cwd=work, check=True, timeout=20)
        if os.environ.get('BSP_UAPI'):
            full_program(work)


def full_program(work):
    """Compile the complete program against BSP headers and simulated devices."""
    binary = work/'full-mini3a'
    wrappers = ['open','close','mmap','munmap','ioctl','poll','pthread_create','fclose']
    command = [os.environ.get('CC','cc'), '-std=gnu11','-Wall','-Wextra','-Werror',
               '-O1','-g','-pthread','-idirafter',os.environ['BSP_UAPI'],
               str(ROOT/'mini3a/mini3a.c'),str(ROOT/'tools/tests/mini3a_fake_device.c'),
               '-o',str(binary)]
    command += ['-Wl,--wrap='+name for name in wrappers]
    if os.environ.get('MINI3A_SANITIZE') == '1':
        command += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run(command,check=True,timeout=60)
    args = [str(binary),'-d','/dev/fake-video','-s','/dev/fake-sensor',
            '-p','/dev/fake-params','-w','4','-h','2']
    cases = [('normal','',60,0),('limit','',3,2),('drift','drift',90,2)]
    cases += [(fault,fault,60,1) for fault in
              ['format','params','mmap','thread','qbuf','dqbuf','fclose','streamoff']]
    for name,fault,count,expected in cases:
        env = dict(os.environ,MINI3A_FAULT=fault)
        output = work/(name+'.nv12')
        result = subprocess.run(args+['-o',str(output),'-n',str(count)],
                                env=env,capture_output=True,text=True,timeout=15)
        assert result.returncode == expected, (name,result.returncode,result.stdout,result.stderr)
        if name=='normal':
            assert len(output.read_bytes())==12 and 'AE/AWB 达到本程序判据' in result.stdout
        if expected:
            assert 'AE/AWB 达到本程序判据' not in result.stdout
        print('PASS complete program:',name,'exit',expected)
    env=dict(os.environ,MINI3A_FAULT='bright')
    p=subprocess.Popen(args+['-o',str(work/'interrupted.nv12'),'-n','100000'],
                       env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        time.sleep(0.1)
        p.terminate()
        stdout,stderr=p.communicate(timeout=15)
        assert p.returncode==143,(p.returncode,stdout,stderr)
        print('PASS complete program: SIGTERM exit 143')
    finally:
        if p.poll() is None:
            p.kill();p.wait()


if __name__ == '__main__':
    main()
