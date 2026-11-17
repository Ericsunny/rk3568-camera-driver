/* Read-only enumeration experiment; records rather than judging each answer. */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/v4l-subdev2";
    const unsigned codes[] = {0x3007, 0};
    const unsigned sizes[][2] = {{0, 0}, {1632, 1224}};
    unsigned c, s, index;
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    for (c = 0; c < 2; c++) {
        for (s = 0; s < 2; s++) {
            for (index = 0; index < 3; index++) {
                struct v4l2_subdev_frame_interval_enum x;
                int rc, error;
                memset(&x, 0, sizeof(x));
                x.pad = 0; x.which = V4L2_SUBDEV_FORMAT_ACTIVE;
                x.code = codes[c]; x.width = sizes[s][0]; x.height = sizes[s][1];
                x.index = index;
                errno = 0;
                rc = ioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL, &x);
                error = rc < 0 ? errno : 0;
                printf("code=0x%04x in=%ux%u index=%u rc=%d errno=%d out=%ux%u code=0x%04x interval=%u/%u\n",
                       codes[c], sizes[s][0], sizes[s][1], index, rc, error,
                       x.width, x.height, x.code, x.interval.numerator, x.interval.denominator);
            }
        }
    }
    close(fd);
    return 0;
}
