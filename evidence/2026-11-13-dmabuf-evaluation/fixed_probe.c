/* Board-only bounded probe: 3 captured buffers held until both MPP contexts
 * are destroyed. Never requeue an input while any encoder can use it. */
#define main evrec_original_main
#include "evrec.c"
#undef main

static int put_one(MppBuffer input, unsigned i, FILE *out)
{
    MppFrame f = NULL;
    MppPacket p = NULL;
    RK_S64 pts = 1000 + (RK_S64)i * 66667;
    int done = 0;
    if (mpp_frame_init(&f) != MPP_OK || !f) return -1;
    mpp_frame_set_width(f, cap.w);
    mpp_frame_set_height(f, cap.h);
    mpp_frame_set_hor_stride(f, cap.stride);
    mpp_frame_set_ver_stride(f, cap.h);
    mpp_frame_set_fmt(f, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(f, input);
    mpp_frame_set_pts(f, pts);
    if (g_mpi->encode_put_frame(g_ctx, f) != MPP_OK) {
        mpp_frame_deinit(&f);
        return -1;
    }
    mpp_frame_deinit(&f);
    while (!done) {
        MPP_RET er = g_mpi->encode_get_packet(g_ctx, &p);
        size_t len;
        if (er != MPP_OK || !p) return -1;
        if (mpp_packet_get_pts(p) != pts) {
            fprintf(stderr, "bad pts at frame %u\n", i);
            mpp_packet_deinit(&p);
            return -1;
        }
        len = mpp_packet_get_length(p);
        done = !mpp_packet_is_partition(p) || mpp_packet_is_eoi(p);
        if (!len || fwrite(mpp_packet_get_pos(p), 1, len, out) != len) {
            mpp_packet_deinit(&p);
            return -1;
        }
        mpp_packet_deinit(&p);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int ids[3] = {-1, -1, -1};
    int fds[3] = {-1, -1, -1};
    MppBuffer bufs[3] = {NULL, NULL, NULL};
    int i, rc = 1;
    FILE *out = NULL;
    if (argc != 2) { fprintf(stderr, "usage: probe /dev/videoX\n"); return 2; }
    if (cap_open(argv[1], 1920, 1080, 4) != 0) goto cleanup;
    if (cap.nbufs < 4) { fprintf(stderr, "need at least 4 capture buffers\n"); goto cleanup; }
    printf("CAP device=%s w=%u h=%u stride=%u nbuf=%d\n", argv[1], cap.w, cap.h, cap.stride, cap.nbufs);
    for (i = 0; i < 3; i++) {
        struct pollfd pf = {cap.fd, POLLIN, 0};
        struct v4l2_plane pl;
        struct v4l2_buffer b;
        struct v4l2_exportbuffer ex;
        memset(&pl, 0, sizeof(pl)); memset(&b, 0, sizeof(b)); memset(&ex, 0, sizeof(ex));
        if (poll(&pf, 1, 3000) <= 0) { fprintf(stderr, "capture poll failed\n"); goto cleanup; }
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP; b.length = 1; b.m.planes = &pl;
        if (xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0 || b.index >= (unsigned)cap.nbufs || pl.data_offset != 0) {
            fprintf(stderr, "DQBUF or layout error\n"); goto cleanup;
        }
        ids[i] = (int)b.index;
        ex.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; ex.index = b.index; ex.plane = 0; ex.flags = O_RDWR | O_CLOEXEC;
        if (xioctl(cap.fd, VIDIOC_EXPBUF, &ex) < 0) { perror("EXPBUF"); goto cleanup; }
        fds[i] = ex.fd;
        printf("HELD frame=%d index=%d len=%zu bytesused=%u offset=%u fd=%d\n", i, ids[i], cap.len[ids[i]], pl.bytesused, pl.data_offset, fds[i]);
    }
    if (enc_init(cap.w, cap.h, cap.stride, 4000, 15, 15) != 0) goto cleanup;
    out = fopen("copy.h264", "wb"); if (!out) goto cleanup;
    for (i = 0; i < 3; i++) {
        memcpy(mpp_buffer_get_ptr(g_fbuf), cap.map[ids[i]], g_fsize);
        if (put_one(g_fbuf, i, out) != 0) { fprintf(stderr, "copy encode failed at %d\n", i); goto cleanup; }
    }
    if (fclose(out) != 0) { out = NULL; goto cleanup; } out = NULL;
    enc_release();
    if (enc_init(cap.w, cap.h, cap.stride, 4000, 15, 15) != 0) goto cleanup;
    for (i = 0; i < 3; i++) {
        MppBufferInfo inf;
        MPP_RET r;
        memset(&inf, 0, sizeof(inf)); inf.type = MPP_BUFFER_TYPE_EXT_DMA;
        inf.size = cap.len[ids[i]]; inf.fd = fds[i]; inf.index = ids[i];
        r = mpp_buffer_import(&bufs[i], &inf);
        printf("IMPORT frame=%d ret=%d buffer=%p\n", i, r, (void*)bufs[i]);
        if (r != MPP_OK || !bufs[i]) goto cleanup;
    }
    out = fopen("dmabuf.h264", "wb"); if (!out) goto cleanup;
    for (i = 0; i < 3; i++) {
        if (put_one(bufs[i], i, out) != 0) { fprintf(stderr, "dmabuf encode failed at %d\n", i); goto cleanup; }
    }
    if (fclose(out) != 0) { out = NULL; goto cleanup; } out = NULL;
    rc = 0;
cleanup:
    if (out) fclose(out);
    /* Crucial: destroy encoder before dropping imported handles or capture. */
    enc_release();
    for (i = 0; i < 3; i++) if (bufs[i]) mpp_buffer_put(bufs[i]);
    for (i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
    cap_release();
    printf("PROBE_RC=%d\n", rc);
    return rc;
}
