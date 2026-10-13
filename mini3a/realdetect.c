/*
 * realdetect.c — mini3a 采集线程 × yolov8 NPU 推理 = 实时检测(步 2 实时版骨架)
 *
 * 数据流:
 *   采集线程(V4L2 MPLANE,DQBUF)──索引──→ 主循环:
 *     1. NV12 → RGB888 转换(手术点 A)          [CPU,全帧最大开销之一]
 *     2. inference_yolov8_model()(已封装好)    [NPU + 内部 letterbox]
 *     3. 后处理结果 → 终端统计 + 画框(可选存图)
 *     4. QBUF 归还
 *
 * 与 mini3a 的关系:采集线程/零拷贝 ring/状态机原样继承;
 * 差别 = 3A 简化为固定曝光(mini3a 已校准的值),焦点全在"接 NPU"。
 *
 * 编译:make  (板上;依赖已全:librknnrt 2.3.2 / librga / stb)
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
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "rknn_api.h"
#include "yolov8.h"
#include "image_utils.h"
#include "image_drawing.h"
#include "file_utils.h"

/* ---------------- 配置 ---------------- */
#define RING_SLOTS   4
#define INF_PERIOD   2            /* 每 N 帧推理一次(帧率不够时跳帧) */
#define DRAW_PERIOD  30           /* 每 N 次推理存一张带框图(看效果) */
#define FIXED_EXP    2242         /* mini3a 校准值:暗房场景 */
#define FIXED_GAIN   32

/* ---------------- V4L2 采集(继承 mini3a,MPLANE) ---------------- */
static char *video_dev = "/dev/video0";
static int frame_w = 1920, frame_h = 1080;
static int vfd = -1, sfd = -1;
static struct buffer { void *start; size_t len; } *bufs;
static int nbufs = RING_SLOTS;
static int ring[RING_SLOTS], ring_head, ring_tail;
static pthread_mutex_t ring_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ring_cv = PTHREAD_COND_INITIALIZER;
static volatile int capture_done = 0;

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

/* 传感器 ctrl(固定值,mini3a 校准过的;一次设置) */
static int set_sensor_ctrl(void)
{
    struct v4l2_ext_controls ctrls;
    struct v4l2_ext_control  c[2];
    memset(&ctrls, 0, sizeof(ctrls));
    memset(c, 0, sizeof(c));
    c[0].id = 0x00980911; c[0].value = FIXED_EXP;
    c[1].id = 0x009e0903; c[1].value = FIXED_GAIN;
    ctrls.count = 2;
    ctrls.controls = c;
    /* 两个 ctrl 不同 class,拆两次发(教训:跨类打包=EINVAL) */
    ctrls.count = 1;
    ctrls.controls = &c[0];
    if (xioctl(sfd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) { perror("exp"); return -1; }
    ctrls.controls = &c[1];
    ctrls.ctrl_class = 0x009e0000;
    if (xioctl(sfd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) { perror("gain"); return -1; }
    return 0;
}

static void *capture_thread(void *arg)
{
    (void)arg;
    while (!capture_done) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[1];
        memset(&b, 0, sizeof(b)); memset(planes, 0, sizeof(planes));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = planes; b.length = 1;
        if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF"); break;
        }
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
    fmt.fmt.pix_mp.width = frame_w;
    fmt.fmt.pix_mp.height = frame_h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    memset(&req, 0, sizeof(req));
    req.count = nbufs; req.type = type; req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }
    nbufs = req.count;
    bufs = (struct buffer *)calloc(nbufs, sizeof(*bufs));
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

/* ================= 手术点 A(你填):NV12 → RGB888 =================
 * demo 的推理入口吃 image_buffer_t(format=IMAGE_FORMAT_RGB888)。
 * 你在手术点 B 里要构造它,这里先把像素转换写好。
 *
 * 公式(BT.601):
 *   R = 1.164(Y-16) + 1.596(V-128)
 *   G = 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)
 *   B = 1.164(Y-16) + 2.018(U-128)
 * 布局:Y 平面 w*h;UV 交织平面 w*h/2(U 偶 V 奇,2x2 共享)。
 * 输出:dst[w*h*3],RGBRGBRGB...
 * 性能提示:内层先取 Y/U/V 再算三个通道;浮点可换整数近似(选修)。
 */
static void nv12_to_rgb888(const uint8_t *src, uint8_t *dst, int w, int h)
{
    /* TODO(你):三重循环或 Y 行外层+X 内层;注意 uv 索引 = (y/2)*(w/2)*2 + (x/2)*2 */
    memset(dst, 128, (size_t)w * h * 3);   /* 占位:中性灰(删除我) */
}

/* ---------------- 推理上下文(封装 demo 接口) ---------------- */
static rknn_app_context_t ctx;

static int npu_init(const char *model_path)
{
    init_post_process();
    if (init_yolov8_model((char *)model_path, &ctx) != 0) {
        printf("init_yolov8_model fail\n");
        return -1;
    }
    printf("NPU 模型就绪:%s\n", model_path);
    return 0;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------------- main:手术点 B(你填)主循环 ---------------- */
int main(int argc, char **argv)
{
    const char *model_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "d:m:")) != -1) {
        switch (opt) {
        case 'd': video_dev = optarg; break;
        case 'm': model_path = optarg; break;
        default:
            fprintf(stderr, "用法:%s -m model/yolov8n_rk3568_i8.rknn [-d /dev/video0]\n", argv[0]);
            return 1;
        }
    }
    if (!model_path) { fprintf(stderr, "必须 -m 指定模型\n"); return 1; }

    sfd = open("/dev/v4l-subdev3", O_RDWR);
    if (sfd < 0) { perror("/dev/v4l-subdev3"); return 1; }
    set_sensor_ctrl();
    if (capture_init() < 0) return 1;
    if (npu_init(model_path) < 0) return 1;

    pthread_t cap_tid;
    pthread_create(&cap_tid, NULL, capture_thread, NULL);
    printf("实时检测开始(每 %d 帧推理一次)\n", INF_PERIOD);

    long f = 0, inf_cnt = 0;
    double t_start = now_ms();
    int run = 1;
    while (run) {
        int idx = -1;
        pthread_mutex_lock(&ring_mu);
        if (ring_tail != ring_head) {
            idx = ring[ring_tail];
            ring_tail = (ring_tail + 1) % RING_SLOTS;
        }
        pthread_mutex_unlock(&ring_mu);
        if (idx < 0) { usleep(5000); continue; }

        f++;

        /* ======== 手术点 B(你填):推理主路径 ========
         * 1) 构造 image_buffer_t src:
         *      image_buffer_t src; memset...
         *      src.width=frame_w; src.height=frame_h;
         *      src.format = IMAGE_FORMAT_RGB888;
         *      src.virt_addr = malloc 后调用 nv12_to_rgb888(...)(别忘了 free)
         * 2) 推理 + 计时:
         *      double t0=now_ms();
         *      object_detect_result_list od;
         *      inference_yolov8_model(&ctx, &src, &od);
         *      double dt = now_ms()-t0; inf_cnt++;
         * 3) 每 10 次推理打印一行 fps 与检测数:
         *      od.count / 最近推理耗时
         * 4) DRAW_PERIOD 到:把画框后的 src 存成 PNG(write_image("det_%ld.png", &src))
         * 5) free(src.virt_addr); QBUF 归还;
         * 6) Ctrl+C(SIGINT)退出:捕获信号置 run=0(提示:sigaction 或简单 flag)
         */

        struct v4l2_buffer b;
        struct v4l2_plane planes[1];
        memset(&b, 0, sizeof(b)); memset(planes, 0, sizeof(planes));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = idx;
        b.m.planes = planes; b.length = 1;
        xioctl(vfd, VIDIOC_QBUF, &b);
    }

    double total = now_ms() - t_start;
    printf("结束:共 %ld 帧,%ld 次推理,平均 fps=%.2f\n",
           f, inf_cnt, inf_cnt * 1000.0 / total);
    deinit_post_process();
    release_yolov8_model(&ctx);
    capture_done = 1;
    pthread_cond_broadcast(&ring_cv);
    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(vfd, VIDIOC_STREAMOFF, &t);
    }
    pthread_join(cap_tid, NULL);
    return 0;
}
