/*
 * mini3a.c — IMX415 用户态 AE/AWB 实验
 *
 * 结构:采集线程(v4l2 完整循环) --buffer索引--> 主线程(3A 控制环)
 * 零拷贝:ring 传递的是 vb2 buffer 的"所有权"(索引),不是像素数据。
 *        采集线程 DQBUF 后不归还,推给主线程;主线程处理完 QBUF 归还。
 *        慢消费 → 采集线程 DQBUF 阻塞 → 自然背压。
 *
 * 编译:make  (板上 gcc 直接编)
 * 节点必须显式指定；不得与 rkaiq 同时控制同一条 ISP 链路。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* vendor UAPI 头使用内核私有 typedef(u8/u32...),用户态需自行补 */
typedef unsigned char u8; typedef signed char s8;
typedef unsigned short u16; typedef signed short s16;
typedef unsigned int u32; typedef signed int s32;
typedef unsigned long long u64; typedef signed long long s64;
#ifndef BIT
#define BIT(x) (1U << (x))
#endif
#ifndef BIT_ULL
#define BIT_ULL(x) (1ULL << (x))
#endif
#include <linux/rkisp21-config.h>
#include <linux/rkisp2-config.h>
/* ---------------- 配置 ---------------- */
#define RING_SLOTS   4          /* 环形队列深度(<= REQBUFS 数) */
#define AE_PERIOD    10         /* 每 N 帧调整一次，给传感器/ISP 留出响应时间 */
#define AE_Y_LO      90         /* 目标带 */
#define AE_Y_HI      130
#define AE_LOCK_FRM  60         /* 连续多少帧带内判 LOCKED */
#define AWB_UV_TOL   24         /* |U-128|+|V-128| 容限 */
static int exp_min, exp_max, gain_min, gain_max;

/* ---------------- 全局状态 ---------------- */
static char  *video_dev;
static char  *subdev_dev;
static char  *params_dev;
static char  *output_path;
static int    frame_w = 1920, frame_h = 1080;
static long   max_frames = 300;                  /* 有限帧数；到上限未收敛返回 2 */
static unsigned frame_stride;
static int streaming;
static volatile sig_atomic_t stop_signal;

static int    vfd = -1;                    /* /dev/video0 */
static int    sfd = -1;                    /* /dev/v4l-subdev3(传感器 ctrl) */
static struct buffer { void *start; size_t len; } *bufs;
static int    nbufs = RING_SLOTS;

/* ring:采集线程推索引,主线程消费 */
static int      ring[RING_SLOTS];
static int      ring_head, ring_tail;
static pthread_mutex_t ring_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ring_cv = PTHREAD_COND_INITIALIZER;
static atomic_int capture_done;
static atomic_int capture_failed;

/* 3A 状态(主线程私有) */
static int cur_exp = 101, cur_gain = 0;
static int ae_locked = 0, awb_locked = 0;
static int ae_hold = 0;                  /* 带内连续帧计数 */

static void on_signal(int sig) { stop_signal = sig; }

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
    struct timespec last_frame, now;
    clock_gettime(CLOCK_MONOTONIC, &last_frame);
    while (!capture_done && !stop_signal) {
        struct pollfd pf = { .fd = vfd, .events = POLLIN };
        struct v4l2_buffer b;
        struct v4l2_plane planes[1];
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((int64_t)(now.tv_sec - last_frame.tv_sec) * 1000000000 +
            now.tv_nsec - last_frame.tv_nsec >= INT64_C(6000000000)) {
            fprintf(stderr, "capture: no frame for 6 seconds\n");
            capture_failed = 1; break;
        }
        int ready = poll(&pf, 1, 200);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) continue;
        if (ready <= 0 || (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "capture poll failed or no frame for 6 seconds\n");
            capture_failed = 1;
            break;
        }
        memset(&b, 0, sizeof(b));
        memset(planes, 0, sizeof(planes));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = planes;
        b.length = 1;
        if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF");
            capture_failed = 1;
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &last_frame);
        if (b.index >= (unsigned)nbufs ||
            planes[0].bytesused < (size_t)frame_stride * frame_h * 3 / 2 ||
            planes[0].data_offset != 0) {
            fprintf(stderr, "invalid/short NV12 capture buffer\n");
            capture_failed = 1;
            break;
        }
        /* 所有权交给主线程;队列满则等(背压) */
        pthread_mutex_lock(&ring_mu);
        while (((ring_head + 1) % RING_SLOTS) == ring_tail && !capture_done)
            pthread_cond_wait(&ring_cv, &ring_mu);
        if (capture_done) { pthread_mutex_unlock(&ring_mu); break; }
        ring[ring_head] = b.index;
        ring_head = (ring_head + 1) % RING_SLOTS;
        pthread_cond_signal(&ring_cv);
        pthread_mutex_unlock(&ring_mu);
    }
    pthread_mutex_lock(&ring_mu);
    capture_done = 1;
    pthread_cond_broadcast(&ring_cv);
    pthread_mutex_unlock(&ring_mu);
    return NULL;
}

/* ---------------- NV12 统计 ----------------
 * frame_stats:从 NV12 帧算 Y/U/V 均值
 *   NV12 布局:前 w*h 字节是 Y 平面;接着 w*h/2 字节是 UV 交织(U,Cb 在前;V,Cr 在后)
 *   返回后 y u v (三个出参) 填 0~255 的均值。U/V 采样:每 4 对取 1(性能)。
 *   自测:对着亮处,Y 应明显大于暗处;U/V 都应接近 128(中性)。
 */
static void frame_stats(const uint8_t *frame, int w, int h, unsigned stride,
                        int *y, int *u, int *v)
{
    uint64_t ysum = 0, usum = 0, vsum = 0;
    const uint8_t *uv;
    int row, x, n = 0;
    if(w <= 0 || h <= 0) {
        *y = 0;
        *u = 128;
        *v = 128;
        return;
    }
    uv = frame + (size_t)stride * h;
    for (row = 0; row < h; row++) {
        for (x = 0; x < w; x++) ysum += frame[(size_t)row * stride + x];
    }
    for (row = 0; row < h / 2; row++) {
        for (x = 0; x < w; x += 8) { /* 每行每 4 对取 1，跳过 padding */
            usum += uv[(size_t)row * stride + x];
            vsum += uv[(size_t)row * stride + x + 1];
            n++;
        }
    }
    *y = (int)(ysum / ((size_t)w * h));
    *u = n ? (int)(usum / n) : 128;
    *v = n ? (int)(vsum / n) : 128;
}

/* ---------------- AE 决策 ----------------
 * ae_step:一步 AE 决策。
 *   输入:测得的 Y 均值
 *   副作用:若需要调节,更新 cur_exp/cur_gain 并调用 ae_apply_ctrl()
 * 参考策略(可改进):Y<LO 且 exp 未到顶 → exp*=1.5;exp 到顶 → gain*=2;
 *                Y>HI → 先降 gain 再降 exp;每次调节后 return 1(表示调了)。
 * 本函数每 AE_PERIOD 帧调用一次。
 */
static int ae_apply_ctrl(void); 
static int ae_step(int y_mean)
{
     int old_exp = cur_exp, old_gain = cur_gain;
     static int limited_warned = 0; 
     int changed = 0;
     const int Y_TARGET = (AE_Y_LO + AE_Y_HI) / 2;  

    if (y_mean < AE_Y_LO) {
        /* Y 值过低，需要增加曝光或增益 */
        if (cur_exp < exp_max) {
            if(y_mean < Y_TARGET / 2) {
                cur_exp = (int)(cur_exp * 2);  
            } else {
               if(y_mean > 0)
                  cur_exp = (int)(cur_exp * Y_TARGET / y_mean); 
            }
            if (cur_exp > exp_max) cur_exp = exp_max;
            changed = 1;
        } else if (cur_gain < gain_max) {
            if (cur_gain == 0) cur_gain = 16;  
            if(y_mean < Y_TARGET / 2) {
                cur_gain = (int)(cur_gain * 2);  
            } else {
                if(y_mean > 0)
                    cur_gain = (int)(cur_gain * Y_TARGET / y_mean); 
            }
            if (cur_gain > gain_max) cur_gain = gain_max;
            changed = 1;
        }
    } else if (y_mean > AE_Y_HI) {
        /* Y 值过高，需要减少曝光或增益 */
        if (cur_gain > gain_min) {
            if(y_mean > Y_TARGET * 2) {
                cur_gain = (int)(cur_gain / 2);  
            } else {
                cur_gain = (int)(cur_gain * Y_TARGET / y_mean); 
            }
            if (cur_gain < gain_min) cur_gain = gain_min;
            changed = 1;
        } else if (cur_exp > exp_min) {
            if(y_mean > Y_TARGET * 2) {
                cur_exp = (int)(cur_exp / 2);  
            } else {
                if(y_mean > 0)
                cur_exp = (int)(cur_exp * Y_TARGET / y_mean); 
            }
            if (cur_exp < exp_min) cur_exp = exp_min;
            changed = 1;
        }
    } else {
        /* Y 值在目标范围内，不需要调节 */
        return 0;
    }
    if (!limited_warned && ((y_mean < AE_Y_LO && cur_exp == exp_max) || (y_mean > AE_Y_HI && cur_gain == gain_min))) {
        printf("Warning: AE reached limit (exp: %d, gain: %d)\n", cur_exp, cur_gain);
        limited_warned = 1;
    }
    if (changed) {
        if (ae_apply_ctrl() < 0) {
            cur_exp = old_exp; cur_gain = old_gain;
            return -1;
        }
        return changed;
    }
    return 0;
}

/* ae_apply_ctrl:将实验范围内的曝光/增益写到显式指定的传感器节点。
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
    r |= set_one_ctrl(sfd, V4L2_CID_EXPOSURE, cur_exp);
    r |= set_one_ctrl(sfd, V4L2_CID_ANALOGUE_GAIN, cur_gain);
    if (r) perror("S_EXT_CTRLS");
    return r;
}

static int read_control(unsigned id, int *value, int *lo, int *hi)
{
    struct v4l2_queryctrl q = { .id = id };
    struct v4l2_ext_control c = { .id = id };
    struct v4l2_ext_controls cs = { .ctrl_class = id & 0xffff0000,
                                    .count = 1, .controls = &c };
    if (xioctl(sfd, VIDIOC_QUERYCTRL, &q) < 0 ||
        q.type != V4L2_CTRL_TYPE_INTEGER || q.step != 1 || q.minimum < 0 ||
        q.maximum < q.minimum || q.maximum > 1000000 ||
        (q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) ||
        xioctl(sfd, VIDIOC_G_EXT_CTRLS, &cs) < 0 ||
        c.value < q.minimum || c.value > q.maximum) {
        fprintf(stderr, "unsupported sensor control 0x%x\n", id); return -1;
    }
    *value = c.value; *lo = q.minimum; *hi = q.maximum;
    return 0;
}

/* AWB:从 NV12 色度偏差估计红/蓝增益，通过 vendor ISP params 下发。
 * 增益 0x100=1.0；这只是场景相关的简单控制实验，不是完整 IQ 算法。
 */
static int cur_gain_r = 0x100, cur_gain_b = 0x100; /* 全局变量,记录当前增益 */
static int awb_apply_params(unsigned int gr, unsigned int gb)
{
    int fd, ret = -1, started = 0;
    size_t maplen = 0;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer b;
    enum v4l2_buf_type t = V4L2_BUF_TYPE_META_OUTPUT;
    struct isp21_isp_params_cfg *cfg;
    void *p = MAP_FAILED;

    fd = open(params_dev, O_RDWR);
    if (fd < 0) return -1;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = t;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) goto out;
    if (fmt.fmt.meta.buffersize != sizeof(*cfg)) {
        fprintf(stderr, "ISP params ABI mismatch: driver=%u program=%zu\n",
                fmt.fmt.meta.buffersize, sizeof(*cfg));
        goto out;
    }

    memset(&req, 0, sizeof(req));
    req.count = 1; req.type = t; req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) goto out;

    memset(&b, 0, sizeof(b));
    b.type = t; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
    if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("ISP params QUERYBUF"); goto out; }
    if (b.length < sizeof(*cfg)) {
        fprintf(stderr, "ISP params buffer too small: QUERYBUF=%u required=%zu (G_FMT=%u)\n",
                b.length, sizeof(*cfg), fmt.fmt.meta.buffersize);
        goto out;
    }
    maplen = b.length;
    p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
    if (p == MAP_FAILED) goto out;

    /* 填 params:只动 AWB_GAIN 一个模块,三组增益全填 */
    cfg = (struct isp21_isp_params_cfg *)p;
    memset(cfg, 0, sizeof(*cfg));
    cfg->module_en_update  = ISP2X_MODULE_AWB_GAIN;
    cfg->module_ens        = ISP2X_MODULE_AWB_GAIN;
    cfg->module_cfg_update = ISP2X_MODULE_AWB_GAIN;
    cfg->others.awb_gain_cfg.gain0_red = gr;  cfg->others.awb_gain_cfg.gain0_blue = gb;
    cfg->others.awb_gain_cfg.gain1_red = gr;  cfg->others.awb_gain_cfg.gain1_blue = gb;
    cfg->others.awb_gain_cfg.gain2_red = gr;  cfg->others.awb_gain_cfg.gain2_blue = gb;
    cfg->others.awb_gain_cfg.gain0_green_r = cfg->others.awb_gain_cfg.gain0_green_b = 0x100;
    cfg->others.awb_gain_cfg.gain1_green_r = cfg->others.awb_gain_cfg.gain1_green_b = 0x100;
    cfg->others.awb_gain_cfg.gain2_green_r = cfg->others.awb_gain_cfg.gain2_green_b = 0x100;

    memset(&b, 0, sizeof(b));
    b.type = t; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
    if (xioctl(fd, VIDIOC_QBUF, &b) < 0) goto out; /* BSP 历史路径使用 bytesused=0 */
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) goto out;
    started = 1;
    usleep(120000);                              /* 等 1-2 帧让内核应用(15fps) */
    ret = 0;
out:
    if (started && xioctl(fd, VIDIOC_STREAMOFF, &t) < 0) ret = -1;
    if (p != MAP_FAILED && munmap(p, maplen) < 0) ret = -1;
    if (close(fd) < 0) ret = -1;
    return ret;
}
static int awb_step(int u_mean, int v_mean)
{
    /* 灰世界:V 偏低=红不足 -> 加 gain_r;U 偏低=蓝不足 -> 加 gain_b。
     * 修了两个坑:
     *   1) 只在 params 真正下发成功后才更新 cur_gain,否则增益"看起来变了"其实没生效
     *   2) 每步限幅 ±20%,避免大幅振荡(u/v 越接近 128,除数越敏感) */
    int changed = 0;
    int gr = cur_gain_r, gb = cur_gain_b;

    if (v_mean > 0 && (v_mean < 128 - AWB_UV_TOL / 2 || v_mean > 128 + AWB_UV_TOL / 2)) {
        int ng = (int)((long)gr * 128 / v_mean);
        if (ng > gr * 6 / 5) ng = gr * 6 / 5;
        if (ng < gr * 4 / 5) ng = gr * 4 / 5;
        if (ng > 0x400) ng = 0x400;
        if (ng < 0x40) ng = 0x40;
        gr = ng; changed = 1;
    }
    if (u_mean > 0 && (u_mean < 128 - AWB_UV_TOL / 2 || u_mean > 128 + AWB_UV_TOL / 2)) {
        int ng = (int)((long)gb * 128 / u_mean);
        if (ng > gb * 6 / 5) ng = gb * 6 / 5;
        if (ng < gb * 4 / 5) ng = gb * 4 / 5;
        if (ng > 0x400) ng = 0x400;
        if (ng < 0x40) ng = 0x40;
        gb = ng; changed = 1;
    }
    if (!changed) return 0;

    if (awb_apply_params(gr, gb) == 0) {
        cur_gain_r = gr; cur_gain_b = gb;
        return 1;
    }
    printf(">>> AWB: params 下发失败,本步不生效\n");
    return -1;
}

/* ---------------- 采集初始化(已写好,读一遍) ---------------- */
static int capture_init(void)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer b;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    vfd = open(video_dev, O_RDWR | O_NONBLOCK);
    if (vfd < 0) { perror(video_dev); return -1; }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix_mp.width = frame_w;
    fmt.fmt.pix_mp.height = frame_h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    printf("fmt: %dx%d fourcc=%c%c%c%c sizeimage=%u\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.pixelformat & 0xff, (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff, (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff,
           fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    frame_w = fmt.fmt.pix_mp.width; frame_h = fmt.fmt.pix_mp.height;
    frame_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
        fmt.fmt.pix_mp.num_planes != 1 || frame_w <= 0 || frame_h <= 0 ||
        frame_w > 8192 || frame_h > 8192 || (frame_w % 2) || (frame_h % 2) ||
        frame_stride < (unsigned)frame_w || frame_stride > 65536) {
        fprintf(stderr, "expected even-sized, single-plane NV12 with valid stride\n");
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.count = nbufs;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }
    nbufs = req.count;
    if (nbufs < 2 || nbufs > 64) { fprintf(stderr, "invalid buffer count\n"); return -1; }
    bufs = calloc(nbufs, sizeof(*bufs));
    if (!bufs) return -1;
    for (int i = 0; i < nbufs; i++) {
        struct v4l2_plane pl[1];
        memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = pl; b.length = 1;
        if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return -1; }
        bufs[i].len = pl[0].length;
        if (bufs[i].len < (size_t)frame_stride * frame_h * 3 / 2) return -1;
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
    streaming = 1;
    return 0;
}

static void capture_request_stop(void)
{
    pthread_mutex_lock(&ring_mu);
    capture_done = 1;
    pthread_cond_broadcast(&ring_cv);
    pthread_mutex_unlock(&ring_mu);
}

static int ring_take(void)
{
    int idx = -1;
    pthread_mutex_lock(&ring_mu);
    if (ring_tail != ring_head) {
        idx = ring[ring_tail];
        ring_tail = (ring_tail + 1) % RING_SLOTS;
        pthread_cond_signal(&ring_cv);
    }
    pthread_mutex_unlock(&ring_mu);
    return idx;
}

static int capture_cleanup(void)
{
    int ret = 0;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (streaming && xioctl(vfd, VIDIOC_STREAMOFF, &type) < 0) ret = -1;
    streaming = 0;
    if (bufs) {
        for (int i = 0; i < nbufs; i++)
            if (bufs[i].start && bufs[i].start != MAP_FAILED &&
                munmap(bufs[i].start, bufs[i].len) < 0) ret = -1;
        free(bufs); bufs = NULL;
    }
    if (vfd >= 0 && close(vfd) < 0) ret = -1;
    if (sfd >= 0 && close(sfd) < 0) ret = -1;
    vfd = sfd = -1;
    return ret;
}

static int save_frame(const uint8_t *data)
{
    FILE *fp = fopen(output_path, "wbx");
    int ret = 0;
    if (!fp) { perror(output_path); return -1; }
    for (int row = 0; row < frame_h * 3 / 2; row++) {
        if (fwrite(data + (size_t)row * frame_stride, 1, frame_w, fp) != (size_t)frame_w) {
            ret = -1; break;
        }
    }
    if (fclose(fp) != 0) ret = -1;
    if (ret) fprintf(stderr, "frame save failed; output may be partial: %s\n", output_path);
    else printf(">>> saved tightly-packed NV12: %s\n", output_path);
    return ret;
}

/* ---------------- 主循环 ---------------- */
int main(int argc, char **argv)
{
    int opt;
    int rc = 0, thread_started = 0;
    long f = 0;
    int saved = 0;
    pthread_t cap_tid;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--help") == 0) {
            printf("用法:%s -d VIDEO -s SENSOR -p PARAMS -o NEW.nv12 [-w W -h H -n N]\n"
                   "节点必须现场确认；与 rkaiq 互斥，勿直接停现役 3A。\n"
                   "退出: 0=达到本程序简单收敛判据并保存；1=错误；2=到帧数上限仍未收敛。\n", argv[0]);
            return 0;
        }
    while ((opt = getopt(argc, argv, "d:s:p:o:w:h:n:")) != -1) {
        switch (opt) {
        case 'd': video_dev = optarg; break;
        case 's': subdev_dev = optarg; break;
        case 'p': params_dev = optarg; break;
        case 'o': output_path = optarg; break;
        case 'w': frame_w = atoi(optarg); break;
        case 'h': frame_h = atoi(optarg); break;
        case 'n': max_frames = atol(optarg); break;
        default:
            fprintf(stderr, "see --help\n");
            return 1;
        }
    }

    if (!video_dev || !subdev_dev || !params_dev || !output_path ||
        frame_w <= 0 || frame_h <= 0 || frame_w > 8192 || frame_h > 8192 ||
        frame_w % 2 || frame_h % 2 || max_frames <= 0 || optind != argc) {
        fprintf(stderr, "explicit -d/-s/-p/-o, even dimensions and positive -n required; see --help\n");
        return 1;
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    sfd = open(subdev_dev, O_RDWR);
    if (sfd < 0) { perror(subdev_dev); return 1; }
    printf(">>> 采集 %s (%dx%d), 传感器 ctrl %s, 上限 %ld 帧\n",
           video_dev, frame_w, frame_h, subdev_dev, max_frames);
    if (capture_init() < 0) { rc = 1; goto cleanup; }
    if (read_control(V4L2_CID_EXPOSURE, &cur_exp, &exp_min, &exp_max) < 0 ||
        read_control(V4L2_CID_ANALOGUE_GAIN, &cur_gain, &gain_min, &gain_max) < 0) {
        rc = 1; goto cleanup;
    }
    if (awb_apply_params(0x100, 0x100) < 0) {
        fprintf(stderr, "cannot establish AWB unity-gain baseline\n");
        rc = 1; goto cleanup;
    }

    int tr = pthread_create(&cap_tid, NULL, capture_thread, NULL);
    if (tr) { fprintf(stderr, "pthread_create: %s\n", strerror(tr)); rc = 1; goto cleanup; }
    thread_started = 1;
    printf("采集线程已启动,3A 控制环开始(目标 Y∈[%d,%d])\n", AE_Y_LO, AE_Y_HI);

    /* 主循环:
     *   每 AE_PERIOD 帧:消费 ring 一帧 → frame_stats → ae_step → awb_step
     *   打印状态行:f=<帧号> Y=.. U=.. V=.. AE=%s AWB=%s
     *   AE/AWB 各自 LOCKED 后停止调节;全 LOCKED → 打印总结,break
     *   处理完的帧必须 QBUF 归还!(所有权转回采集线程)
     */
    int y = 0, u = 128, v = 128;
    while ((!ae_locked || !awb_locked) && f < max_frames && !stop_signal) {
        int idx = ring_take();
        if (idx < 0) {
            if (capture_done) { rc = 1; break; }
            usleep(10000); continue;
        }

        f++;
        frame_stats(bufs[idx].start, frame_w, frame_h, frame_stride, &y, &u, &v);

        if (f % AE_PERIOD == 0) {
            /* 锁定后仍观察后续帧；AE 调整可能使先前的 AWB 判据失效。 */
            if (y < AE_Y_LO || y > AE_Y_HI) { ae_locked = 0; ae_hold = 0; }
            if (abs(u - 128) + abs(v - 128) > AWB_UV_TOL) awb_locked = 0;
            if (!ae_locked && ae_step(y) < 0) { rc = 1; break; }
            if (!awb_locked && y > 60) {
                int changed = awb_step(u, v);      
                if (changed < 0) { rc = 1; break; }
                int err = abs(u - 128) + abs(v - 128);
                if (!changed && err <= AWB_UV_TOL) {
                    awb_locked = 1;
                    printf(">>> AWB LOCKED at f=%ld (u=%d v=%d)\n", f, u, v);
                }
            }
           
            /* 连续检查点位于亮度目标带内，才标记 AE 锁定。 */
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

        /* 收官帧落盘:双锁定(或到 -n 上限)这一帧就是闭环结果(此刻 idx 还没归还,像素有效) */
        if (((ae_locked && awb_locked) || (max_frames > 0 && f >= max_frames)) && !saved) {
            if (save_frame(bufs[idx].start) < 0) { rc = 1; break; }
            saved = 1;
        }
        /* 处理完成后归还采集 buffer。 */
        struct v4l2_buffer b;
        struct v4l2_plane planes[1];
        memset(&b, 0, sizeof(b)); memset(planes, 0, sizeof(planes));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = idx;
        b.m.planes = planes; b.length = 1;
        if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); rc = 1; break; }
    }
    if (capture_failed) rc = 1;
    if (!rc && (!ae_locked || !awb_locked)) rc = 2;
    printf(">>> 最终: f=%ld Y=%d(dY=%+d) U=%d(dU=%+d) V=%d(dV=%+d) AE=%s AWB=%s 增益 g_r=0x%X g_b=0x%X\n",
           f, y, y - 110, u, u - 128, v, v - 128,
           ae_locked ? "LOCKED" : "RUNNING", awb_locked ? "LOCKED" : "RUNNING",
           cur_gain_r, cur_gain_b);

cleanup:
    capture_request_stop();
    if (thread_started && pthread_join(cap_tid, NULL) != 0) rc = 1;
    if (capture_failed) rc = 1;
    if (capture_cleanup() < 0) rc = 1;
    if (stop_signal) return 128 + stop_signal;
    if (!rc && saved) printf("AE/AWB 达到本程序判据,共 %ld 帧。\n", f);
    else fprintf(stderr, "not converged or failed: rc=%d, frames=%ld\n", rc, f);
    return rc;
}
