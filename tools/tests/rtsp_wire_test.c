/* Verify the named RTP constants retain their existing wire bytes. */
#include "../rtsp_hub.c"

static void check(int ok, const char *msg)
{
	if (!ok) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

static void read_exact(int fd, uint8_t *buf, size_t len)
{
	check(recv(fd, buf, len, MSG_WAITALL) == (ssize_t)len, "receive RTP packet");
}

int main(void)
{
	int s[2];
	struct rtsp_cli c = {0};
	uint8_t short_nal[] = {0x65, 0x11, 0x22};
	uint8_t long_nal[1401], buf[4 + 12 + 1400];
	struct txdiag dg;
	int i;

	check(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0, "socketpair");
	c.fd = s[0]; c.ch = 0; c.seq = 0x1234;
	check(rtsp_send_nal(&c, short_nal, sizeof(short_nal), 0x01020304, 1, &dg) == 0,
	      "send short NAL");
	read_exact(s[1], buf, 19);
	check(!memcmp(buf, "$\x00\x00\x0f\x80\xe0\x12\x34\x01\x02\x03\x04\x00\x00\x00\x00\x65\x11\x22", 19),
	      "short NAL exact wire bytes");
	for (i = 0; i < (int)sizeof(long_nal); i++) long_nal[i] = (uint8_t)i;
	long_nal[0] = 0x65;
	check(rtsp_send_nal(&c, long_nal, sizeof(long_nal), 0x01020304, 1, &dg) == 0,
	      "send fragmented NAL");
	read_exact(s[1], buf, 4 + 12 + 1400);
	check(buf[0] == '$' && buf[2] == 0x05 && buf[3] == 0x84 &&
	      buf[4] == 0x80 && buf[5] == 96 && buf[6] == 0x12 && buf[7] == 0x35 &&
	      buf[16] == 0x7c && buf[17] == 0x85 &&
	      !memcmp(buf + 18, long_nal + 1, 1398), "first FU-A exact bytes");
	read_exact(s[1], buf, 4 + 12 + 4);
	check(buf[0] == '$' && buf[2] == 0 && buf[3] == 16 &&
	      buf[4] == 0x80 && buf[5] == 0xe0 && buf[6] == 0x12 && buf[7] == 0x36 &&
	      buf[16] == 0x7c && buf[17] == 0x45 &&
	      !memcmp(buf + 18, long_nal + 1399, 2), "last FU-A exact bytes");
	close(s[0]); close(s[1]);
	puts("RTP single and fragmented NAL bytes: PASS");
	return 0;
}
