/*
 * params_poke.c — 通过 /dev/video8(params, META_OUTPUT)下发 AWB 数字增益
 *
 * 通路:用户态填 struct isp21_isp_params_cfg(RK1P = 结构体直接 dump)
 *       → QBUF 提交 → 内核 isp_params_v21.c 帧边界调用 awbgain_config()
 *       → 寄存器 0x138/13c/140/144/148/14c 生效(rkaiq 同款官方通路)。
 *
 * 用法:
 *   1) 另一终端先开采集流(v4l2-ctl --stream-mmap ... 或 mini3a)
 *   2) ./params_poke -g 0x130 -b 0x120            # 常驻循环提交
 *      ./params_poke -g 0x130 -b 0x120 -f 60      # 提交 60 帧后退出
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
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

#define PARAMS_DEV "/dev/video8"
#define NBUFS      4

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static void fill_awb_gain(struct isp21_isp_params_cfg *p,
                          unsigned int gr, unsigned int gb)
{
    p->module_en_update  = ISP2X_MODULE_AWB_GAIN;
    p->module_ens       |= ISP2X_MODULE_AWB_GAIN;
    p->module_cfg_update = ISP2X_MODULE_AWB_GAIN;
    p->others.awb_gain_cfg.gain0_red     = (uint16_t)gr;
    p->others.awb_gain_cfg.gain0_green_r = 0x100;
    p->others.awb_gain_cfg.gain0_blue    = (uint16_t)gb;
    p->others.awb_gain_cfg.gain0_green_b = 0x100;
    p->others.awb_gain_cfg.gain1_red     = (uint16_t)gr;
    p->others.awb_gain_cfg.gain1_green_r = 0x100;
    p->others.awb_gain_cfg.gain1_blue    = (uint16_t)gb;
    p->others.awb_gain_cfg.gain1_green_b = 0x100;
    p->others.awb_gain_cfg.gain2_red     = (uint16_t)gr;
    p->others.awb_gain_cfg.gain2_green_r = 0x100;
    p->others.awb_gain_cfg.gain2_blue    = (uint16_t)gb;
    p->others.awb_gain_cfg.gain2_green_b = 0x100;
}

static struct v4l2_buffer bufs[NBUFS];   /* 保存 offset/length */
static void *mmaps[NBUFS];

int main(int argc, char **argv)
{
    unsigned int gr = 0x100, gb = 0x100;
    long frames = 0;
    int fd, i;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_META_OUTPUT;

    for (int o; (o = getopt(argc, argv, "g:b:f:")) != -1; ) {
        switch (o) {
        case 'g': gr = (unsigned int)strtoul(optarg, NULL, 0); break;
        case 'b': gb = (unsigned int)strtoul(optarg, NULL, 0); break;
        case 'f': frames = atol(optarg); break;
        default:
            fprintf(stderr, "用法:%s -g <gain_r_hex> -b <gain_b_hex> [-f 帧数,0=常驻]\n", argv[0]);
            return 1;
        }
    }

    fd = open(PARAMS_DEV, O_RDWR);
    if (fd < 0) { perror(PARAMS_DEV); return 1; }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
    printf("params fmt: buffersize=%u (需要 %zu)\n",
           fmt.fmt.meta.buffersize, sizeof(struct isp21_isp_params_cfg));

    memset(&req, 0, sizeof(req));
    req.count = NBUFS;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }

    for (i = 0; i < (int)req.count; i++) {
        memset(&bufs[i], 0, sizeof(bufs[i]));
        bufs[i].type = type; bufs[i].memory = V4L2_MEMORY_MMAP; bufs[i].index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &bufs[i]) < 0) { perror("QUERYBUF"); return 1; }
        mmaps[i] = mmap(NULL, bufs[i].length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, bufs[i].m.offset);
        if (mmaps[i] == MAP_FAILED) { perror("mmap"); return 1; }
        memset(mmaps[i], 0, bufs[i].length);
    }
    for (i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
    }
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
    printf("params 流已开,开始提交:gain_r=0x%X gain_b=0x%X frames=%ld\n", gr, gb, frames);

    for (long n = 0; frames == 0 || n < frames; n++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = type; b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) { usleep(4000); continue; }
            perror("DQBUF"); return 1;
        }
        fill_awb_gain((struct isp21_isp_params_cfg *)mmaps[b.index], gr, gb);
        ((struct isp21_isp_params_cfg *)mmaps[b.index])->frame_id = (uint32_t)n;
        /* bytesused 保持 0:实测设置 10475 会被拒(EINVAL),
         * 初始排队(bytesused=0)成功且内核按完整 buffer 应用 */
        b.bytesused = 0;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
        if (n % 30 == 0) printf("已提交 %ld 帧 params(g_r=0x%X g_b=0x%X)\n", n, gr, gb);
    }

    printf("提交完成,退出\n");
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    return 0;
}
