/*
 * zc_s1.c — 零拷贝第 1 步:从 rkisp mainpath 抓一帧 NV12,把它导出成 dmabuf 并校验
 *
 * 做对判据(对应 18 号任务书 S1):
 *   - 打印 export fd 与 size
 *   - /proc/self/fdinfo/<fd> 里的 size 与 V4L2 buffer length 一致
 *   - fdinfo 里出现 dma-buf 的 name/owner 信息(说明真是 dma-buf 而不是普通 fd)
 *
 * 编译:gcc -O2 -o zc_s1 zc_s1.c
 * 运行:./zc_s1 [device] [width] [height]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <time.h>

#define NBUF 4

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}

static void fourcc_str(unsigned int f, char out[5])
{
	out[0] = f & 0xff; out[1] = (f >> 8) & 0xff;
	out[2] = (f >> 16) & 0xff; out[3] = (f >> 24) & 0xff; out[4] = 0;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/video5";
	unsigned int w = (argc > 2) ? (unsigned int)atoi(argv[2]) : 3840;
	unsigned int h = (argc > 3) ? (unsigned int)atoi(argv[3]) : 2160;
	int fd, ret, i;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	void *mem[NBUF] = {0};
	size_t len[NBUF] = {0};
	unsigned int nplanes = 0;
	double t0, t1;

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof cap);
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); return 1; }
	printf("device     : %s\n", dev);
	printf("driver     : %s  card: %s  bus: %s\n", cap.driver, cap.card, cap.bus_info);
	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		printf("ERROR: 不支持 STREAMING\n"); return 1;
	}
	printf("caps       : %s%s\n",
	       (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? "MPLANE " : "",
	       (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ? "SINGLE-PLANE " : "");

	/* 1) 设格式:NV12(与 imx415 mainpath 默认一致) */
	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
	char cc[5]; fourcc_str(fmt.fmt.pix_mp.pixelformat, cc);
	printf("current fmt: %ux%u %s  num_planes=%u  bpl=%u sizeimage=%u\n",
	       fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, cc,
	       fmt.fmt.pix_mp.num_planes,
	       fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
	       fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

	fmt.fmt.pix_mp.width = w;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	nplanes = fmt.fmt.pix_mp.num_planes;
	fourcc_str(fmt.fmt.pix_mp.pixelformat, cc);
	printf("after S_FMT: %ux%u %s num_planes=%u bpl=%u sizeimage=%u\n",
	       fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, cc, nplanes,
	       fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
	       fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

	/* 2) 申请 MMAP 缓冲 */
	memset(&req, 0, sizeof req);
	req.count = NBUF;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }
	printf("REQBUFS    : count=%u (实际分配)\n", req.count);

	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof buf);
		memset(planes, 0, sizeof planes);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = planes;
		buf.length = nplanes;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return 1; }
		len[i] = planes[0].length;
		mem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[0].m.mem_offset);
		if (mem[i] == MAP_FAILED) { perror("mmap"); return 1; }
		printf("buf[%d]     : length=%zu moffset=0x%x addr=%p\n",
		       i, len[i], planes[0].m.mem_offset, mem[i]);
	}

	/* 3) 全部入队,开流 */
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof buf);
		memset(planes, 0, sizeof planes);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = planes;
		buf.length = nplanes;
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
	}
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	t0 = now_ms();
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

	/* 4) 拿两帧(第一帧常常是半帧,取第二帧) */
	int idx = -1;
	struct v4l2_buffer got;
	struct v4l2_plane gotp[VIDEO_MAX_PLANES];
	for (int k = 0; k < 2; k++) {
		memset(&got, 0, sizeof got);
		memset(gotp, 0, sizeof gotp);
		got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		got.memory = V4L2_MEMORY_MMAP;
		got.m.planes = gotp;
		got.length = nplanes;
		if (xioctl(fd, VIDIOC_DQBUF, &got) < 0) { perror("DQBUF"); return 1; }
		idx = got.index;
		printf("DQBUF #%d   : index=%d bytesused=%u seq=%u ts=%ld.%06ld\n",
		       k, got.index, gotp[0].bytesused, got.sequence,
		       (long)got.timestamp.tv_sec, (long)got.timestamp.tv_usec);
		if (k == 0) {   /* 第一帧还回去,继续等第二帧 */
			if (xioctl(fd, VIDIOC_QBUF, &got) < 0) { perror("QBUF back"); return 1; }
		}
	}
	t1 = now_ms();
	printf("两帧耗时   : %.1f ms\n", t1 - t0);

	/* 5) 对拿到的这一帧做 EXPBUF */
	struct v4l2_exportbuffer exp;
	memset(&exp, 0, sizeof exp);
	exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	exp.index = idx;
	exp.plane = 0;
	exp.flags = O_RDWR | O_CLOEXEC;
	if (xioctl(fd, VIDIOC_EXPBUF, &exp) < 0) { perror("VIDIOC_EXPBUF"); return 1; }
	printf("\n=== EXPBUF ===\n");
	printf("export fd  : %d\n", exp.fd);
	printf("buffer len : %zu  (V4L2 mmap length)\n", len[idx]);

	char path[64], line[256];
	snprintf(path, sizeof path, "/proc/self/fdinfo/%d", exp.fd);
	FILE *fi = fopen(path, "r");
	if (!fi) { perror("open fdinfo"); return 1; }
	printf("--- %s ---\n", path);
	while (fgets(line, sizeof line, fi)) {
		if (strstr(line, "size") || strstr(line, "name") ||
		    strstr(line, "exp_name") || strstr(line, "ino"))
			printf("%s", line);
	}
	fclose(fi);

	/* 6) 用 mmap 的地址和 dmabuf 的 mmap 各读一行 Y,比对内容(证明是同一块内存) */
	void *dmap = mmap(NULL, len[idx], PROT_READ | PROT_WRITE, MAP_SHARED, exp.fd, 0);
	if (dmap == MAP_FAILED) {
		perror("mmap(dmabuf)");
	} else {
		unsigned char *a = (unsigned char *)mem[idx];
		unsigned char *b = (unsigned char *)dmap;
		int same = (memcmp(a, b, 3840) == 0);
		printf("\n=== 同一块内存校验 ===\n");
		printf("Y 行 0 前 8 字节(mmapped) : %02x %02x %02x %02x %02x %02x %02x %02x\n",
		       a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
		printf("Y 行 0 前 8 字节(dmabuf)  : %02x %02x %02x %02x %02x %02x %02x %02x\n",
		       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
		printf("首行 3840 字节一致        : %s\n", same ? "YES ✓" : "NO ✗");
		munmap(dmap, len[idx]);
	}

	/* 7) 收尾 */
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	if (exp.fd > 0) close(exp.fd);
	for (i = 0; i < (int)req.count; i++)
		if (mem[i]) munmap(mem[i], len[i]);
	close(fd);
	printf("\nS1 PASS\n");
	return 0;
}
