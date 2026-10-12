/*
 * mini3a.c — 用户态 3A 闭环(教学版骨架)
 *
 * 结构:采集线程(v4l2 完整循环) --buffer索引--> 主线程(3A 控制环)
 * 零拷贝:ring 传递的是 vb2 buffer 的"所有权"(索引),不是像素数据。
 *        采集线程 DQBUF 后不归还,推给主线程;主线程处理完 QBUF 归还。
 *        慢消费 → 采集线程 DQBUF 阻塞 → 自然背压。
 *
 * 你要填的 6 个手术点(搜 "TODO(你)"):frame_stats / ae_step / awb_step /
 *        ae_apply_ctrl 的 ioctl 组装 / 主循环接线 / 收敛状态机。
 * 编译:make  (板上 gcc 直接编)
 * 用法:./mini3a -d /dev/video0 -w 1920 -h 1080 [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* ---------------- 配置 ---------------- */
#define RING_SLOTS   4          /* 环形队列深度(<= REQBUFS 数) */
#define AE_PERIOD    10         /* 每 N 帧调一次 AE(预测题 2) */
#define AE_Y_LO      90         /* 目标带 */
#define AE_Y_HI      130
#define AE_LOCK_FRM  60         /* 连续多少帧带内判 LOCKED */
#define AWB_UV_TOL   24         /* |U-128|+|V-128| 容限 */
#define EXP_MIN      4
#define EXP_MAX      2242
#define GAIN_MAX     240

/* ---------------- 全局状态 ---------------- */
static char  *video_dev = "/dev/video0";
static int    frame_w = 1920, frame_h = 1080;
static int    verbose = 0;

static int    vfd = -1;                    /* /dev/video0 */
static int    sfd = -1;                    /* /dev/v4l-subdev3(传感器 ctrl) */
static struct buffer { void *start; size_t len; } *bufs;
static int    nbufs = RING_SLOTS;

/* ring:采集线程推索引,主线程消费 */
static int      ring[RING_SLOTS];
static int      ring_head, ring_tail;
static pthread_mutex_t ring_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ring_cv = PTHREAD_COND_INITIALIZER;
static volatile int    capture_done = 0;

/* 3A 状态(主线程私有) */
static int cur_exp = 101, cur_gain = 0;
static int ae_locked = 0, awb_locked = 0;
static int ae_hold = 0;                  /* 带内连续帧计数 */

/* ---------------- 工具 ---------------- */
static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

/* ---------------- 采集线程 ---------------- */
static void *capture_thread(void *arg)
{
    (void)arg;
    while (!capture_done) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[1];
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = planes;
        b.length = 1;
        if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF");
            break;
        }
        /* 所有权交给主线程;队列满则等(背压) */
        pthread_mutex_lock(&ring_mu);
        while (((ring_head + 1) % RING_SLOTS) == ring_tail && !capture_done)
            pthread_cond_wait(&ring_cv, &ring_mu);
        ring[ring_head] = b.index;
        ring_head = (ring_head + 1) % RING_SLOTS;
        pthread_cond_signal(&ring_cv);
        pthread_mutex_unlock(&ring_mu);
    }
    return NULL;
}

/* ================= 手术点 2(你填) =================
 * frame_stats:从 NV12 帧算 Y/U/V 均值
 *   NV12 布局:前 w*h 字节是 Y 平面;接着 w*h/2 字节是 UV 交织(U,Cb 在前;V,Cr 在后)
 *   返回后 y u v (三个出参) 填 0~255 的均值。U/V 采样:每 4 对取 1(性能)。
 *   自测:对着亮处,Y 应明显大于暗处;U/V 都应接近 128(中性)。
 */
static void frame_stats(const uint8_t *frame, int w, int h,
                        int *y, int *u, int *v)
{
    /* TODO(你):实现并删掉下面三行占位 */
    uint64_t ysum = 0, usum = 0, vsum = 0;
    const uint8_t *uv = frame + (uint32_t)w * h;
    int i, n = 0;
    if(w <= 0 || h <= 0) {
        *y = 0;
        *u = 128;
        *v = 128;
        return;
    }
    for (i = 0; i < w * h; i++) {
        ysum += frame[i];
    }
    for (i = 0; i < w * h / 2; i += 8) { /* 每 4 对取 1 */
        usum += uv[i];
        vsum += uv[i + 1];
        n++;
    }
    *y = (int)ysum / ((uint32_t)(w * h));
    *u = n ? (int)(usum / n) : 128;
    *v = n ? (int)(vsum / n) : 128;
}

/* ================= 手术点 3(你填) =================
 * ae_step:一步 AE 决策。
 *   输入:测得的 Y 均值
 *   副作用:若需要调节,更新 cur_exp/cur_gain 并调用 ae_apply_ctrl()
 * 参考策略(可改进):Y<LO 且 exp 未到顶 → exp*=1.5;exp 到顶 → gain*=2;
 *                Y>HI → 先降 gain 再降 exp;每次调节后 return 1(表示调了)。
 * 记得:本函数被每 AE_PERIOD 帧调用一次(为什么,预测题 2)。
 */
static int ae_apply_ctrl(void); 
static int ae_step(int y_mean)
{
    /* TODO(你):实现策略;调节成功返回 1,不动返回 0 */
     static int limited_warned = 0; 
     int changed = 0;
     const int Y_TARGET = (AE_Y_LO + AE_Y_HI) / 2;  

    if (y_mean < AE_Y_LO) {
        /* Y 值过低，需要增加曝光或增益 */
        if (cur_exp < EXP_MAX) {
            if(y_mean < Y_TARGET / 2) {
                cur_exp = (int)(cur_exp * 2);  
            } else {
               if(y_mean > 0)
                  cur_exp = (int)(cur_exp * Y_TARGET / y_mean); 
            }
            if (cur_exp > EXP_MAX) cur_exp = EXP_MAX;
            changed = 1;
        } else if (cur_gain < GAIN_MAX) {
            if (cur_gain == 0) cur_gain = 16;  
            if(y_mean < Y_TARGET / 2) {
                cur_gain = (int)(cur_gain * 2);  
            } else {
                if(y_mean > 0)
                    cur_gain = (int)(cur_gain * Y_TARGET / y_mean); 
            }
            if (cur_gain > GAIN_MAX) cur_gain = GAIN_MAX;
            changed = 1;
        }
    } else if (y_mean > AE_Y_HI) {
        /* Y 值过高，需要减少曝光或增益 */
        if (cur_gain > 0) {
            if(y_mean > Y_TARGET * 2) {
                cur_gain = (int)(cur_gain / 2);  
            } else {
                cur_gain = (int)(cur_gain * Y_TARGET / y_mean); 
            }
            changed = 1;
        } else if (cur_exp > EXP_MIN) {
            if(y_mean > Y_TARGET * 2) {
                cur_exp = (int)(cur_exp / 2);  
            } else {
                if(y_mean > 0)
                cur_exp = (int)(cur_exp * Y_TARGET / y_mean); 
            }
            if (cur_exp < EXP_MIN) cur_exp = EXP_MIN;
            changed = 1;
        }
    } else {
        /* Y 值在目标范围内，不需要调节 */
        return 0;
    }
    if (!limited_warned && ((y_mean < AE_Y_LO && cur_exp == EXP_MAX) || (y_mean > AE_Y_HI && cur_gain == 0))) {
        printf("Warning: AE reached limit (exp: %d, gain: %d)\n", cur_exp, cur_gain);
        limited_warned = 1;
    }
    if (changed) {
        ae_apply_ctrl();
        return changed;
    }
    return 0;
}

/* ae_apply_ctrl:把 cur_exp/cur_gain 写到传感器(v4l-subdev3)
 * ioctl 骨架已给全,你只需按注释组装 ext_controls(这是 V4L2 ctrl 的标准姿势)。
 */
static int set_one_ctrl(int fd, unsigned int id, int value) {
    
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control  c;
    memset(&ctrls, 0, sizeof(ctrls));
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.value = value;
    ctrls.ctrl_class = id & 0xffff0000;  /* 高 16 位是 class */
    ctrls.count = 1;
    ctrls.controls = &c;
    return xioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0 ? -1 : 0;
}
static int ae_apply_ctrl(void)
{
    int r = 0;
    r |= set_one_ctrl(sfd, 0x00980911, cur_exp);
    r |= set_one_ctrl(sfd, 0x009e0903, cur_gain);
    if (r) perror("S_EXT_CTRLS");
    return r;
}

/* ================= 手术点 4(你填) =================
 * awb_step:灰世界 UV 校正。
 *   U/V 是相对 128 的偏移:V 偏高 = 画面偏红 → 减 gain_r / 加 gain_b。
 *   定点增益:0x100 = 1.0x;写法:
 *     FILE *f = fopen("/sys/module/isp_wb/parameters/gain_r", "w");
 *     fprintf(f, "0x%X\n", gr); fclose(f);
 *   提示:先 insmod /tmp/isp_wb.ko(或从 VM ~/ispwb/ 传)。
 *   clamp 增益到 [0x40, 0x400]。
 */
static int cur_gain_r = 0x100, cur_gain_b = 0x100; /* 全局变量,记录当前增益 */
static void write_gain(const char *name, int val) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/module/isp_wb/parameters/%s", name);
    FILE *f = fopen(path, "w");
    if(!f) {
        perror(path);
        return;
    }
    fprintf(f, "0x%X\n", val);
    fclose(f);
}
static int awb_step(int u_mean, int v_mean)
{
    /* TODO(你):实现;调节成功返回 1 */
    int changed = 0;
    if(v_mean < 128 - AWB_UV_TOL / 2 || v_mean > 128 + AWB_UV_TOL / 2) {
        cur_gain_r = cur_gain_r * 128 /v_mean;
        if (cur_gain_r > 0x400) cur_gain_r = 0x400;
        if (cur_gain_r < 0x40) cur_gain_r = 0x40;
        write_gain("gain_r", cur_gain_r);
        changed = 1;
    }
    if(u_mean < 128 - AWB_UV_TOL / 2 || u_mean > 128 + AWB_UV_TOL / 2) {
        cur_gain_b = cur_gain_b * 128 /u_mean;
        if (cur_gain_b > 0x400) cur_gain_b = 0x400;
        if (cur_gain_b < 0x40) cur_gain_b = 0x40;
        write_gain("gain_b", cur_gain_b);
        changed = 1;
    }
    return changed;
}

/* ---------------- 采集初始化(已写好,读一遍) ---------------- */
static int capture_init(void)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer b;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    vfd = open(video_dev, O_RDWR);
    if (vfd < 0) { perror(video_dev); return -1; }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix.width = frame_w;
    fmt.fmt.pix.height = frame_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    printf("fmt: %dx%d fourcc=%c%c%c%c sizeimage=%u\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.pixelformat & 0xff, (fmt.fmt.pix.pixelformat >> 8) & 0xff,
           (fmt.fmt.pix.pixelformat >> 16) & 0xff, (fmt.fmt.pix.pixelformat >> 24) & 0xff,
           fmt.fmt.pix.sizeimage);

    memset(&req, 0, sizeof(req));
    req.count = nbufs;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }
    nbufs = req.count;
    bufs = calloc(nbufs, sizeof(*bufs));
    for (int i = 0; i < nbufs; i++) {
        struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = pl; b.length = 1;
        if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return -1; }
        bufs[i].len = pl[0].length;
        bufs[i].start = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, vfd, pl[0].m.mem_offset);
        if (bufs[i].start == MAP_FAILED) { perror("mmap"); return -1; }
    }
    for (int i = 0; i < nbufs; i++) {
        struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = pl; b.length = 1;
        if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return -1; }
    }
    if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return -1; }
    return 0;
}

static void capture_stop(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    capture_done = 1;
    pthread_cond_broadcast(&ring_cv);
    xioctl(vfd, VIDIOC_STREAMOFF, &type);
}

/* ---------------- main:手术点 5(你填主循环) ---------------- */
int main(int argc, char **argv)
{
    int opt;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--help") == 0) {
            printf("用法:%s -d /dev/video0 -w 1920 -h 1080 [-v] [--help]\n", argv[0]);
            return 0;
        }
    while ((opt = getopt(argc, argv, "d:w:h:v")) != -1) {
        switch (opt) {
        case 'd': video_dev = optarg; break;
        case 'w': frame_w = atoi(optarg); break;
        case 'h': frame_h = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "用法:%s -d /dev/video0 -w 1920 -h 1080 [-v]\n", argv[0]);
            return 1;
        }
    }

    sfd = open("/dev/v4l-subdev3", O_RDWR);
    if (sfd < 0) { perror("/dev/v4l-subdev3"); return 1; }
    if (capture_init() < 0) return 1;

    pthread_t cap_tid;
    pthread_create(&cap_tid, NULL, capture_thread, NULL);
    printf("采集线程已启动,3A 控制环开始(目标 Y∈[%d,%d])\n", AE_Y_LO, AE_Y_HI);

    /* ======== 手术点 5(你填):3A 主循环 ========
     * 骨架给出循环变量与退出条件,你填 3A 步进逻辑:
     *   每 AE_PERIOD 帧:消费 ring 一帧 → frame_stats → ae_step → awb_step
     *   打印状态行:f=<帧号> Y=.. U=.. V=.. AE=%s AWB=%s
     *   AE/AWB 各自 LOCKED 后停止调节;全 LOCKED → 打印总结,break
     *   处理完的帧必须 QBUF 归还!(所有权转回采集线程)
     */
    long f = 0;
    int y = 0, u = 128, v = 128;
    while (!ae_locked || !awb_locked) {
        int idx = -1;
        pthread_mutex_lock(&ring_mu);
        if (ring_tail != ring_head) {
            idx = ring[ring_tail];
            ring_tail = (ring_tail + 1) % RING_SLOTS;
        }
        pthread_mutex_unlock(&ring_mu);
        if (idx < 0) { usleep(10000); continue; }

        f++;
        frame_stats(bufs[idx].start, frame_w, frame_h, &y, &u, &v);   /* 手术点 2 */

        if (f % AE_PERIOD == 0) {
            if (!ae_locked) ae_step(y);                               /* 手术点 3 */
            if (!awb_locked && y > 60) {
                int changed = awb_step(u, v);      
                int err = abs(u - 128) + abs(v - 128);
                if (!changed && err < AWB_UV_TOL) {
                    awb_locked = 1;
                    printf(">>> AWB LOCKED at f=%ld (u=%d v=%d)\n", f, u, v);
                }                         /* 手术点 4 */
            }                      /* 手术点 4 */
           
            /* 手术点 5:收敛状态机(注意:ae_hold 逻辑必须在 if(f%10) 块内,
             * 且 else 归属于"是否在带内"这个 if——花括号配对错了计数永远清零) */
            if (!ae_locked){
                if (y >= AE_Y_LO && y <= AE_Y_HI){
                    ae_hold++;
                    if (ae_hold >= AE_LOCK_FRM / AE_PERIOD){
                    ae_locked = 1;
                    printf(">>> AE LOCKED at f=%ld (y=%d)\n", f, y);
                   }
                } else{
                ae_hold = 0;
                }
            }
            printf("f=%ld Y=%d U=%d V=%d AE=%s AWB=%s\n", f, y, u, v,
                   ae_locked ? "LOCKED" : "RUNNING",
                   awb_locked ? "LOCKED" : "RUNNING");
        }

        /* 归还 buffer 给驱动(所有权转回;漏掉这步 → 预测题 1) */
        struct v4l2_buffer b;
        struct v4l2_plane planes[1];
        memset(&b, 0, sizeof(b)); memset(planes, 0, sizeof(planes));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = idx;
        b.m.planes = planes; b.length = 1;
        xioctl(vfd, VIDIOC_QBUF, &b);
    }
    printf("AE+AWB 双收敛,共 %ld 帧。检查 /root/frame_v6.nv12 对比图,收工!\n", f);

    capture_stop();
    pthread_join(cap_tid, NULL);
    return 0;
}
