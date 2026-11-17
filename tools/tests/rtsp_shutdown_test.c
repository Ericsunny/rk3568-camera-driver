/* Linux socketpair regression: stop in the middle of a fragmented H.264 NAL. */
#include "../rtsp_hub.c"
#ifndef RTSP_STOP_GRACE_MS
#define RTSP_STOP_GRACE_MS 500
#define TEST_OLD_SHUTDOWN 1
#endif

#define TEST_NAL_BYTES (256 * 1024)
static uint8_t test_nal[TEST_NAL_BYTES];
static uint8_t received_nal[TEST_NAL_BYTES];
static size_t received_bytes;
static int sender_rc, saw_end, saw_marker;

static void check(int ok, const char *why)
{
	if (!ok) { fprintf(stderr, "FAIL %s\n", why); exit(1); }
}

static int read_exact(int fd, uint8_t *buf, size_t size)
{
	size_t got = 0;
	while (got < size) {
		ssize_t n = recv(fd, buf + got, size - got, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return got ? -1 : 0;
		got += (size_t)n;
	}
	return 1;
}

static int read_packet(int fd)
{
	uint8_t hdr[4], pkt[12 + RTP_H264_PAYLOAD_MAX];
	int rc = read_exact(fd, hdr, sizeof(hdr));
	size_t size, payload;
	if (rc <= 0) return rc;
	check(hdr[0] == '$', "interleaved header");
	size = ((unsigned)hdr[2] << 8) | hdr[3];
	check(size >= 14 && size <= sizeof(pkt), "packet size");
	check(read_exact(fd, pkt, size) == 1, "complete interleaved packet");
	check((pkt[12] & 0x1f) == 28, "FU-A packet");
	if (pkt[13] & 0x80) {
		check(received_bytes == 0, "one start");
		received_nal[received_bytes++] = (pkt[12] & 0xe0) | (pkt[13] & 0x1f);
	}
	payload = size - 14;
	check(received_bytes + payload <= sizeof(received_nal), "payload bound");
	memcpy(received_nal + received_bytes, pkt + 14, payload);
	received_bytes += payload;
	saw_end |= !!(pkt[13] & 0x40);
	saw_marker |= !!(pkt[1] & 0x80);
	return 1;
}

static void *sender(void *arg)
{
	struct rtsp_cli *client = arg;
	struct txdiag diag;
	int fd = client->fd;
	sender_rc = rtsp_send_nal(client, test_nal, sizeof(test_nal), 90000, 1, &diag);
	pthread_mutex_lock(&g_rtsp_lock);
	client->playing = 0;
	client->fd = -1;
	client->th_done = 1;
	pthread_mutex_unlock(&g_rtsp_lock);
	shutdown(fd, SHUT_RDWR);
	close(fd);
	return NULL;
}

static void *receiver(void *arg)
{
	int fd = *(int *)arg;
	usleep(50 * 1000);
	while (read_packet(fd) == 1) { }
	return NULL;
}

static void run_case(int drain)
{
	int pair[2], small = 4096;
	pthread_t reader;
	uint64_t start, elapsed;
	struct timeval timeout = { .tv_sec = 2 };

	check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
	check(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0,
	      "small send buffer");
	check(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
	      "receive timeout");
	memset(g_cli, 0, sizeof(g_cli));
	g_rtsp_stopping = 0;
	g_hub_stopping = 0;
#ifndef TEST_OLD_SHUTDOWN
	atomic_store(&g_rtsp_finish_by_ms, 0);
#endif
	g_rtsp_lfd = socket(AF_INET, SOCK_STREAM, 0);
	check(g_rtsp_lfd >= 0, "placeholder listener");
	g_cli[0].used = g_cli[0].playing = 1;
	g_cli[0].fd = pair[0];
	received_bytes = 0;
	saw_end = saw_marker = 0;
	sender_rc = 99;
	check(pthread_create(&g_cli[0].tid, NULL, sender, &g_cli[0]) == 0, "sender thread");
	check(read_packet(pair[1]) == 1 && received_bytes < sizeof(test_nal),
	      "stop after first fragment, before NAL end");
	if (drain)
		check(pthread_create(&reader, NULL, receiver, &pair[1]) == 0, "receiver thread");
	start = now_ms_mono();
	rtsp_stop();
	elapsed = now_ms_mono() - start;
	check(elapsed <= RTSP_STOP_GRACE_MS + 1000, "bounded shutdown");
	if (drain) {
		pthread_join(reader, NULL);
		check(sender_rc == RTSP_TX_OK, "in-flight NAL finishes");
		check(received_bytes == sizeof(test_nal) && saw_end && saw_marker,
		      "FU-A end and RTP marker arrive");
		check(memcmp(received_nal, test_nal, sizeof(test_nal)) == 0,
		      "complete exact NAL payload");
	} else {
		check(sender_rc == RTSP_TX_FATAL, "stalled receiver fails by deadline");
	}
	close(pair[1]);
	printf("shutdown drain=%d elapsed_ms=%llu: PASS\n", drain,
	       (unsigned long long)elapsed);
}

int main(void)
{
	size_t i;
	for (i = 0; i < sizeof(test_nal); i++) test_nal[i] = (uint8_t)i;
	test_nal[0] = 0x65;
	run_case(1);
	run_case(0);
	return 0;
}
