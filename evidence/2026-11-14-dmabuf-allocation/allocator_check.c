#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <rockchip/mpp_buffer.h>

int main(int argc, char **argv)
{
    MppBufferGroup group = NULL;
    MppBuffer buffer = NULL;
    struct dma_buf_sync sync;
    struct v4l2_requestbuffers req;
    char path[80], line[256];
    FILE *info = NULL;
    int fd = -1, allocated = 0, rc = 1;
    if (argc != 2) return 2;
    if (mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION) != MPP_OK ||
        mpp_buffer_get(group, &buffer, 65536) != MPP_OK) goto done;
    if (mpp_buffer_get_fd(buffer) < 0 || !mpp_buffer_get_ptr(buffer)) goto done;
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", mpp_buffer_get_fd(buffer));
    info = fopen(path, "r");
    if (!info) goto done;
    while (fgets(line, sizeof(line), info)) fputs(line, stdout);
    if (ferror(info)) goto done;
    if (fclose(info)) { info = NULL; goto done; }
    info = NULL;
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    if (ioctl(mpp_buffer_get_fd(buffer), DMA_BUF_IOCTL_SYNC, &sync) < 0) goto done;
    memset(mpp_buffer_get_ptr(buffer), 0x80, 65536);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    if (ioctl(mpp_buffer_get_fd(buffer), DMA_BUF_IOCTL_SYNC, &sync) < 0) goto done;
    fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) goto done;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    req.count = 4;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) goto done;
    allocated = 1;
    printf("REQBUFS DMABUF count=%u; no QBUF/STREAMON performed\n", req.count);
    rc = req.count ? 0 : 1;
done:
    if (rc) fprintf(stderr, "allocator check failed errno=%d (%s)\n", errno, strerror(errno));
    if (info && fclose(info)) rc = 1;
    if (allocated) {
        req.count = 0;
        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) rc = 1;
    }
    if (fd >= 0 && close(fd)) rc = 1;
    if (buffer && mpp_buffer_put(buffer) != MPP_OK) rc = 1;
    if (group && mpp_buffer_group_put(group) != MPP_OK) rc = 1;
    return rc;
}
