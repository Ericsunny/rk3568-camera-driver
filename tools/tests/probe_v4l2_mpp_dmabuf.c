/* Isolated live compare with CPU OSD and DMA-BUF CPU synchronization.
 * No diagnostic full-frame memcmp in timing; streams compared after capture.
 * Detection, RTSP and event writer disabled. Not production evrec. */
#define main evrec_original_main
#include "../evrec.c"
#undef main
#include <rockchip/mpp_task.h>
#include <rockchip/mpp_meta.h>
#include <linux/dma-buf.h>

struct probe_enc {
    MppCtx ctx;
    MppApi *mpi;
    MppBufferGroup grp;
    MppBuffer copy_buf, packet_buf;
    MppTask input, spare[32];
    MppFrame inflight_frame;
    MppPacket inflight_packet;
    unsigned nspare;
    size_t frame_size;
};

static int probe_init(struct probe_enc *e, unsigned w, unsigned h, unsigned stride, int fps)
{
    memset(e, 0, sizeof(*e));
    if (enc_init(w, h, stride, 4000, fps, fps) != 0) { enc_release(); return -1; }
    e->ctx = g_ctx; e->mpi = g_mpi; e->grp = g_grp; e->copy_buf = g_fbuf; e->frame_size = g_fsize;
    g_ctx = NULL; g_mpi = NULL; g_grp = NULL; g_fbuf = NULL;
    if (mpp_buffer_get(e->grp, &e->packet_buf, e->frame_size) != MPP_OK) return -1;
    if (e->mpi->poll(e->ctx, MPP_PORT_INPUT, (MppPollType)2000) != MPP_OK ||
        e->mpi->dequeue(e->ctx, MPP_PORT_INPUT, &e->input) != MPP_OK || !e->input) return -1;
    return 0;
}

static void probe_release(struct probe_enc *e)
{
    if (e->ctx) { e->mpi->reset(e->ctx); mpp_destroy(e->ctx); e->ctx = NULL; }
    if (e->inflight_frame) mpp_frame_deinit(&e->inflight_frame);
    if (e->inflight_packet) mpp_packet_deinit(&e->inflight_packet);
    if (e->packet_buf) mpp_buffer_put(e->packet_buf);
    if (e->copy_buf) mpp_buffer_put(e->copy_buf);
    if (e->grp) mpp_buffer_group_put(e->grp);
    memset(e, 0, sizeof(*e));
}

static int probe_one(struct probe_enc *e, MppBuffer input, RK_S64 pts, FILE *output)
{
    MppTask submitted = NULL, out_task = NULL, task = NULL;
    MppFrame ret_frame = NULL;
    MppPacket ret_pkt = NULL;
    MPP_RET r;
    size_t len;
    int i;
    if (!e->input || mpp_frame_init(&e->inflight_frame) != MPP_OK ||
        mpp_packet_init_with_buffer(&e->inflight_packet, e->packet_buf) != MPP_OK) return -1;
    mpp_packet_set_length(e->inflight_packet, 0);
    mpp_frame_set_width(e->inflight_frame, cap.w); mpp_frame_set_height(e->inflight_frame, cap.h);
    mpp_frame_set_hor_stride(e->inflight_frame, cap.stride); mpp_frame_set_ver_stride(e->inflight_frame, cap.h);
    mpp_frame_set_fmt(e->inflight_frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(e->inflight_frame, input); mpp_frame_set_pts(e->inflight_frame, pts);
    if (mpp_task_meta_set_frame(e->input, KEY_INPUT_FRAME, e->inflight_frame) != MPP_OK ||
        mpp_task_meta_set_packet(e->input, KEY_OUTPUT_PACKET, e->inflight_packet) != MPP_OK) return -1;
    submitted = e->input; e->input = NULL;
    if (e->mpi->enqueue(e->ctx, MPP_PORT_INPUT, submitted) != MPP_OK) return -1;
    if (e->mpi->poll(e->ctx, MPP_PORT_OUTPUT, (MppPollType)2000) != MPP_OK ||
        e->mpi->dequeue(e->ctx, MPP_PORT_OUTPUT, &out_task) != MPP_OK || !out_task) return -1;
    r = mpp_task_meta_get_packet(out_task, KEY_OUTPUT_PACKET, &ret_pkt);
    if (r != MPP_OK || !ret_pkt || ret_pkt != e->inflight_packet) return -1;
    len = mpp_packet_get_length(ret_pkt);
    if (!len || mpp_packet_get_pts(ret_pkt) != pts || fwrite(mpp_packet_get_pos(ret_pkt), 1, len, output) != len) return -1;
    if (e->mpi->enqueue(e->ctx, MPP_PORT_OUTPUT, out_task) != MPP_OK) return -1;
    for (i = 0; i < 32; i++) {
        if (e->mpi->poll(e->ctx, MPP_PORT_INPUT, (MppPollType)2000) != MPP_OK ||
            e->mpi->dequeue(e->ctx, MPP_PORT_INPUT, &task) != MPP_OK || !task) return -1;
        if (task != submitted) {
            if (e->nspare >= 32) return -1;
            e->spare[e->nspare++] = task;
            continue;
        }
        if (mpp_task_meta_get_frame(task, KEY_INPUT_FRAME, &ret_frame) != MPP_OK ||
            ret_frame != e->inflight_frame || mpp_frame_get_pts(ret_frame) != pts ||
            mpp_frame_get_buffer(ret_frame) != input) return -1;
        e->input = task;
        mpp_frame_deinit(&e->inflight_frame);
        mpp_packet_deinit(&e->inflight_packet);
        return 0;
    }
    return -1;
}


/* MPP owns capture buffers; V4L2 imports their DMA-BUF fds. */
static MppBufferGroup capture_group;
static MppBuffer capture_buffers[8];
static int capture_on;

static int queue_import(unsigned index)
{
    struct v4l2_buffer b;
    struct v4l2_plane p;
    memset(&b, 0, sizeof(b)); memset(&p, 0, sizeof(p));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF; b.index = index;
    b.length = 1; b.m.planes = &p;
    p.m.fd = mpp_buffer_get_fd(capture_buffers[index]);
    p.length = (unsigned)cap.len[index];
    if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {
        fprintf(stderr, "import QBUF index=%u fd=%d length=%u errno=%d\n",
                index, p.m.fd, p.length, errno);
        return -1;
    }
    return 0;
}

static int capture_import_open(const char *device, unsigned w, unsigned h)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    unsigned i;
    cap.fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (cap.fd < 0) return -1;
    memset(&fmt, 0, sizeof(fmt)); fmt.type = type;
    fmt.fmt.pix_mp.width = w; fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(cap.fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return -1; }
    printf("FORMAT %ux%u planes=%u stride=%u size=%u\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.num_planes,
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline, fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    if (fmt.fmt.pix_mp.width != w || fmt.fmt.pix_mp.height != h ||
        fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
        fmt.fmt.pix_mp.num_planes != 1) { errno = EINVAL; return -1; }
    cap.w = w; cap.h = h; cap.stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (cap.stride < w || fmt.fmt.pix_mp.plane_fmt[0].sizeimage <
        (size_t)cap.stride * h * 3 / 2) { errno = EINVAL; return -1; }
    memset(&req, 0, sizeof(req)); req.type = type;
    req.memory = V4L2_MEMORY_DMABUF; req.count = 4;
    if (xioctl(cap.fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS DMABUF"); return -1; }
    printf("REQBUFS DMABUF returned %u\n", req.count);
    if (req.count < 3 || req.count > 8) { errno = EINVAL; return -1; }
    cap.nbufs = (int)req.count;
    if (mpp_buffer_group_get_internal(&capture_group, MPP_BUFFER_TYPE_ION) != MPP_OK) return -1;
    for (i = 0; i < req.count; ++i) {
        struct v4l2_buffer b;
        struct v4l2_plane p;
        memset(&b, 0, sizeof(b)); memset(&p, 0, sizeof(p));
        b.type = type; b.memory = V4L2_MEMORY_DMABUF; b.index = i;
        b.length = 1; b.m.planes = &p;
        if (xioctl(cap.fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF DMABUF"); return -1; }
        /* The BSP queue aligns allocation height beyond S_FMT sizeimage. */
        cap.len[i] = p.length;
        if (cap.len[i] < fmt.fmt.pix_mp.plane_fmt[0].sizeimage) { errno = EINVAL; return -1; }
        printf("BUFFER index=%u queue_length=%zu\n", i, cap.len[i]);
        if (mpp_buffer_get(capture_group, &capture_buffers[i], cap.len[i]) != MPP_OK) return -1;
        cap.map[i] = mpp_buffer_get_ptr(capture_buffers[i]);
        if (!cap.map[i] || mpp_buffer_get_fd(capture_buffers[i]) < 0 || queue_import(i) < 0) return -1;
    }
    if (xioctl(cap.fd, VIDIOC_STREAMON, &type) < 0) return -1;
    capture_on = 1;
    return 0;
}

static int capture_import_close(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_requestbuffers req;
    int rc = 0, i;
    if (cap.fd >= 0) {
        if (capture_on && xioctl(cap.fd, VIDIOC_STREAMOFF, &type) < 0) rc = -1;
        capture_on = 0;
        memset(&req, 0, sizeof(req)); req.type = type; req.memory = V4L2_MEMORY_DMABUF;
        if (xioctl(cap.fd, VIDIOC_REQBUFS, &req) < 0) rc = -1;
        if (close(cap.fd)) rc = -1;
        cap.fd = -1;
    }
    for (i = 0; i < 8; ++i) {
        if (capture_buffers[i] && mpp_buffer_put(capture_buffers[i]) != MPP_OK) rc = -1;
        capture_buffers[i] = NULL; cap.map[i] = NULL; cap.len[i] = 0;
    }
    if (capture_group && mpp_buffer_group_put(capture_group) != MPP_OK) rc = -1;
    capture_group = NULL; cap.nbufs = 0;
    return rc;
}

static int cpu_sync(int fd, unsigned long long flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    int ret;
    do { ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); }
    while (ret < 0 && (errno == EINTR || errno == EAGAIN));
    return ret;
}

int main(int argc, char **argv)
{
    struct probe_enc copy = {0}, direct = {0};
    FILE *fa = NULL, *fb = NULL;
    int w, h, fps, frames, i, rc = 1, returned = 0, qbuf_ok = 0, sync_ok = 0;
    double copy_cpu = 0, dma_cpu = 0, t0;
    if (argc != 6) { fprintf(stderr, "usage: probe DEVICE WIDTH HEIGHT FPS FRAMES\n"); return 2; }
    w = atoi(argv[2]); h = atoi(argv[3]); fps = atoi(argv[4]); frames = atoi(argv[5]);
    if (w<=0 || h<=0 || fps<=0 || frames<1 || frames>300) return 2;
    if (capture_import_open(argv[1], (unsigned)w, (unsigned)h) != 0) { perror("capture_import_open"); goto done; }
    if (cap.w != (unsigned)w || cap.h != (unsigned)h || cap.nbufs < 3) goto done;
    if (probe_init(&copy, cap.w, cap.h, cap.stride, fps) != 0 ||
        probe_init(&direct, cap.w, cap.h, cap.stride, fps) != 0) goto done;
    fa=fopen("copy.h264","wb"); fb=fopen("dmabuf.h264","wb"); if (!fa || !fb) goto done;
    printf("START device=%s width=%u height=%u stride=%u buffers=%d frames=%d\n",argv[1],cap.w,cap.h,cap.stride,cap.nbufs,frames); fflush(stdout);
    for (i=0; i<frames; i++) {
        struct pollfd pf={cap.fd,POLLIN,0}; struct v4l2_plane pl; struct v4l2_buffer b;
        RK_S64 pts=1000+(RK_S64)i*1000000/fps;
        memset(&pl,0,sizeof(pl)); memset(&b,0,sizeof(b));
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=V4L2_MEMORY_DMABUF; b.length=1; b.m.planes=&pl;
        if (poll(&pf,1,3000)<=0 || xioctl(cap.fd,VIDIOC_DQBUF,&b)<0 ||
            b.index >= (unsigned)cap.nbufs || (b.flags & V4L2_BUF_FLAG_ERROR) || pl.data_offset != 0) { fprintf(stderr,"capture failed frame=%d\n",i); goto done; }
        if (cpu_sync(mpp_buffer_get_fd(capture_buffers[b.index]), DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW) < 0) { perror("CPU_START"); goto done; }
        t0=now_ms(CLOCK_PROCESS_CPUTIME_ID);
        memcpy(mpp_buffer_get_ptr(copy.copy_buf),cap.map[b.index],copy.frame_size);
        g_osd_seq=(unsigned)i; g_osd_ms=(double)i*1000.0/fps;
        draw_osd(mpp_buffer_get_ptr(copy.copy_buf), (int)cap.stride, (int)cap.w, (int)cap.h);
        if (probe_one(&copy,copy.copy_buf,pts,fa)!=0) { fprintf(stderr,"copy task failed frame=%d\n",i); goto done; }
        copy_cpu += now_ms(CLOCK_PROCESS_CPUTIME_ID)-t0;
        t0=now_ms(CLOCK_PROCESS_CPUTIME_ID);
        draw_osd(cap.map[b.index], (int)cap.stride, (int)cap.w, (int)cap.h);
        if (cpu_sync(mpp_buffer_get_fd(capture_buffers[b.index]), DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW) < 0) {
            perror("CPU_END"); goto done;
        }
        sync_ok++;
        if (probe_one(&direct,capture_buffers[b.index],pts,fb)!=0) { fprintf(stderr,"dmabuf task failed frame=%d\n",i); goto done; }
        dma_cpu += now_ms(CLOCK_PROCESS_CPUTIME_ID)-t0;
        returned++;
        if (queue_import(b.index)<0) { perror("QBUF"); goto done; }
        qbuf_ok++;
        if ((i+1)%10==0) { printf("PROGRESS frames=%d returned=%d qbuf=%d\n",i+1,returned,qbuf_ok); fflush(stdout); }
    }
    if (fflush(fa)!=0 || fflush(fb)!=0) goto done;
    rc=0;
done:
    if (fa && fclose(fa)!=0) rc=1;
    if (fb && fclose(fb)!=0) rc=1;
    /* No QBUF on errors: complete/reset both contexts before releasing fds. */
    probe_release(&direct); probe_release(&copy);
    if (capture_import_close() != 0) rc = 1;
    printf("RESULT rc=%d returned=%d qbuf=%d sync_pairs=%d copy_cpu_ms=%.3f dma_cpu_ms=%.3f\n",rc,returned,qbuf_ok,sync_ok,copy_cpu,dma_cpu); fflush(stdout);
    return rc;
}
