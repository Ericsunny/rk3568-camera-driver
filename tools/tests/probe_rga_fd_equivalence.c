/* Compare virtual-address and DMA-BUF fd RGA preprocessing on the same frame. */
#define main evrec_original_main
#include "../evrec.c"
#undef main

int main(int argc, char **argv)
{
    int fds[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
    unsigned char *reference = NULL;
    size_t rgb_size;
    int w, h, frames, i, j, compared = 0, differences = 0, rc = 1;
    if (argc != 6) return 2;
    w = atoi(argv[2]); h = atoi(argv[3]); frames = atoi(argv[5]);
    if (w <= 0 || h <= 0 || frames < 1 || frames > 120) return 2;
    if (cap_open(argv[1], w, h, 4) < 0) return 1;
    rgb_size = (size_t)g_npu_w * g_npu_h * 3;
    g_rgb = malloc(rgb_size);
    reference = malloc(rgb_size);
    if (!g_rgb || !reference || rga_init() < 0) goto done;
    for (i = 0; i < cap.nbufs; i++) {
        struct v4l2_exportbuffer ex;
        memset(&ex, 0, sizeof(ex)); ex.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ex.index = i; ex.plane = 0; ex.flags = O_RDWR | O_CLOEXEC;
        if (xioctl(cap.fd, VIDIOC_EXPBUF, &ex) < 0) { perror("EXPBUF"); goto done; }
        fds[i] = ex.fd;
    }
    for (i = 0; i < frames; i++) {
        struct pollfd pfd = { .fd = cap.fd, .events = POLLIN };
        struct v4l2_buffer b;
        struct v4l2_plane pl;
        memset(&b, 0, sizeof(b)); memset(&pl, 0, sizeof(pl));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP; b.length = 1; b.m.planes = &pl;
        if (poll(&pfd, 1, 3000) <= 0 || xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0 ||
            b.index >= (unsigned)cap.nbufs || (b.flags & V4L2_BUF_FLAG_ERROR)) goto done;
        if (rga_scale(cap.map[b.index], cap.w, cap.h, cap.stride, -1) != 0) goto done;
        memcpy(reference, g_rgb, rgb_size);
        if (rga_scale(cap.map[b.index], cap.w, cap.h, cap.stride, fds[b.index]) != 0) goto done;
        if (memcmp(reference, g_rgb, rgb_size)) differences++;
        compared++;
        if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) goto done;
    }
    rc = differences ? 1 : 0;
done:
    for (j = 0; j < 8; j++) if (fds[j] >= 0) close(fds[j]);
    cap_release();
    free(reference); free(g_rgb); free(g_rga_buf);
    printf("RESULT rc=%d compared=%d different=%d bytes_per_rgb=%zu\n", rc, compared, differences, rgb_size);
    return rc;
}
