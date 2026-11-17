/* Isolated fixed-frame microbenchmark, no camera buffer is requeued until
 * ALL MPP contexts have been destroyed. Not a continuous-service benchmark. */
#define main evrec_original_main
#include "evrec.c"
#undef main

static int encode_one(MppBuffer buf, int i, FILE *out, int fps)
{
    MppFrame f = NULL; MppPacket p = NULL;
    RK_S64 pts = 1000 + (RK_S64)i * 1000000 / fps;
    int done = 0;
    if (mpp_frame_init(&f) != MPP_OK || !f) return -1;
    mpp_frame_set_width(f, cap.w); mpp_frame_set_height(f, cap.h);
    mpp_frame_set_hor_stride(f, cap.stride); mpp_frame_set_ver_stride(f, cap.h);
    mpp_frame_set_fmt(f, MPP_FMT_YUV420SP); mpp_frame_set_buffer(f, buf); mpp_frame_set_pts(f, pts);
    if (g_mpi->encode_put_frame(g_ctx, f) != MPP_OK) { mpp_frame_deinit(&f); return -1; }
    mpp_frame_deinit(&f);
    while (!done) {
        MPP_RET er = g_mpi->encode_get_packet(g_ctx, &p); size_t len;
        if (er != MPP_OK || !p) return -1;
        if (mpp_packet_get_pts(p) != pts) { mpp_packet_deinit(&p); return -1; }
        len = mpp_packet_get_length(p);
        done = !mpp_packet_is_partition(p) || mpp_packet_is_eoi(p);
        if (!len || fwrite(mpp_packet_get_pos(p), 1, len, out) != len) { mpp_packet_deinit(&p); return -1; }
        mpp_packet_deinit(&p);
    }
    return 0;
}

static int run_one(int idx, int direct, int fd, int capture_index, int fps, int n)
{
    MppBuffer imported = NULL; FILE *out = NULL; char name[64];
    int i, rc = 1; double c0, w0, cpu, wall;
    if (enc_init(cap.w, cap.h, cap.stride, 4000, fps, fps) != 0) goto done;
    if (direct) {
        MppBufferInfo inf; MPP_RET r;
        memset(&inf, 0, sizeof(inf)); inf.type = MPP_BUFFER_TYPE_EXT_DMA;
        inf.size = cap.len[capture_index]; inf.fd = fd; inf.index = capture_index;
        r = mpp_buffer_import(&imported, &inf);
        if (r != MPP_OK || !imported) { fprintf(stderr, "IMPORT_FAIL run=%d ret=%d\n",idx,r); goto done; }
    }
    snprintf(name, sizeof(name), "run%02d-%s.h264", idx, direct ? "dmabuf" : "copy");
    out = fopen(name, "wb"); if (!out) goto done;
    c0 = now_ms(CLOCK_PROCESS_CPUTIME_ID); w0 = now_ms(CLOCK_MONOTONIC);
    for (i = 0; i < n; i++) {
        if (!direct) memcpy(mpp_buffer_get_ptr(g_fbuf), cap.map[capture_index], g_fsize);
        if (encode_one(direct ? imported : g_fbuf, i, out, fps) != 0) {
            fprintf(stderr, "ENCODE_FAIL run=%d frame=%d\n", idx, i); goto done;
        }
    }
    if (fflush(out) != 0) goto done;
    cpu = now_ms(CLOCK_PROCESS_CPUTIME_ID) - c0; wall = now_ms(CLOCK_MONOTONIC) - w0;
    if (fclose(out) != 0) { out = NULL; goto done; } out = NULL;
    printf("RUN index=%d mode=%s frames=%d cpu_ms=%.3f wall_ms=%.3f copy_bytes=%zu\n",
           idx, direct ? "dmabuf" : "copy", n, cpu, wall, direct ? (size_t)0 : g_fsize*(size_t)n);
    fflush(stdout); rc = 0;
done:
    if (out) fclose(out);
    enc_release();
    if (imported) mpp_buffer_put(imported);
    return rc;
}

int main(int argc, char **argv)
{
    int fd = -1, idx = -1, i, rc = 1, width, height, fps, n;
    static const int modes[6] = {0, 1, 1, 0, 0, 1};
    struct pollfd pf; struct v4l2_plane pl; struct v4l2_buffer b; struct v4l2_exportbuffer ex;
    if (argc != 6) { fprintf(stderr, "usage: probe DEVICE WIDTH HEIGHT FPS FRAMES\n"); return 2; }
    width = atoi(argv[2]); height = atoi(argv[3]); fps = atoi(argv[4]); n = atoi(argv[5]);
    if (width <= 0 || height <= 0 || fps <= 0 || n < 1 || n > 120) return 2;
    if (cap_open(argv[1], width, height, 4) != 0) goto done;
    if (cap.w != (unsigned)width || cap.h != (unsigned)height) goto done;
    pf.fd = cap.fd; pf.events = POLLIN; pf.revents = 0;
    if (poll(&pf, 1, 3000) <= 0) goto done;
    memset(&pl, 0, sizeof(pl)); memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.length = 1; b.m.planes = &pl;
    if (xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0 || b.index >= (unsigned)cap.nbufs || pl.data_offset != 0) goto done;
    idx = (int)b.index;
    memset(&ex, 0, sizeof(ex)); ex.type = b.type; ex.index = b.index; ex.flags = O_RDWR | O_CLOEXEC;
    if (xioctl(cap.fd, VIDIOC_EXPBUF, &ex) < 0) goto done;
    fd = ex.fd;
    printf("HELD device=%s width=%u height=%u stride=%u index=%d len=%zu fd=%d\n", argv[1],cap.w,cap.h,cap.stride,idx,cap.len[idx],fd);
    fflush(stdout);
    for (i=0; i<6; i++) if (run_one(i, modes[i], fd, idx, fps, n) != 0) goto done;
    rc = 0;
done:
    enc_release();
    if (fd >= 0) close(fd);
    cap_release();
    printf("PERF_PROBE_RC=%d\n",rc); fflush(stdout);
    return rc;
}
