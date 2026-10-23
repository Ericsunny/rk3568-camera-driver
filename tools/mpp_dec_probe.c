/*
 * mpp_dec_probe.c —— 用 MPP 解码 .h264,验证 RTSP 抓下来的码流真的能解
 * 采用 ParserSplit 模式:整段码流一次性丢给解码器,由硬件解析器自己切 NAL。
 * 编译: gcc -O2 -o mpp_dec_probe mpp_dec_probe.c -lrockchip_mpp
 * 用法: ./mpp_dec_probe <in.h264> [max_frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "用法: %s <in.h264> [max]\n", argv[0]); return 2; }
	int maxf = (argc > 2) ? atoi(argv[2]) : 0;
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror("打开"); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc(sz);
	if (fread(buf, 1, sz, f) != (size_t)sz) return 1;
	fclose(f);
	printf(">>> 码流 %ld 字节\n", sz);

	/* 从第一个 SPS 开始(抓包可能起于 GOP 中间) */
	long off = 0;
	for (long i = 0; i + 5 < sz; i++) {
		if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1 && (buf[i+3] & 0x1f) == 7) { off = i; break; }
		if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1 && (buf[i+4] & 0x1f) == 7) { off = i; break; }
	}
	printf(">>> 首个 SPS 偏移 %ld\n", off);
	unsigned char *p0 = buf + off;
	long n0 = sz - off;

	MppCtx ctx; MppApi *mpi;
	if (mpp_create(&ctx, &mpi) != MPP_OK) return 1;
	if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC) != MPP_OK) return 1;

	RK_U32 need_split = 1;
	mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &need_split);

	MppPacket pkt;
	mpp_packet_init(&pkt, p0, n0);
	mpp_packet_set_pos(pkt, p0);
	mpp_packet_set_length(pkt, n0);

	int got = 0, idle_rounds = 0, put_done = 0, flush_done = 0;
	for (int rounds = 0; rounds < 500; rounds++) {
		if (!put_done && mpi->decode_put_packet(ctx, pkt) == MPP_OK) put_done = 1;
		if (put_done && !flush_done) {   /* 空包 + EOS 让解码器吐出最后一张 AU */
			MppPacket ep = NULL;
			mpp_packet_init(&ep, NULL, 0);
			mpp_packet_set_eos(ep);
			if (mpi->decode_put_packet(ctx, ep) == MPP_OK) flush_done = 1;
		}

		int idle = 1;
		for (;;) {
			MppFrame fr = NULL;
			if (mpi->decode_get_frame(ctx, &fr) != MPP_OK || !fr) break;
			idle = 0;
			if (mpp_frame_get_info_change(fr)) {
				printf(">>> 分辨率变更 %dx%d\n", mpp_frame_get_width(fr), mpp_frame_get_height(fr));
				mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
				mpp_frame_deinit(&fr);
				continue;
			}
			if (mpp_frame_get_eos(fr)) { mpp_frame_deinit(&fr); goto out; }
			if (got == 0) {
				MppBuffer b = mpp_frame_get_buffer(fr);
				if (b) {
					unsigned char *px = mpp_buffer_get_ptr(b);
					int n = mpp_frame_get_width(fr) * mpp_frame_get_height(fr);
					long sum = 0; int c = 0;
					for (int i = 0; i < n; i += 16) { sum += px[i]; c++; }
					printf(">>> 首帧 %dx%d Y 采样均值 %.1f\n", mpp_frame_get_width(fr),
					       mpp_frame_get_height(fr), (double)sum / c);
				}
			}
			got++;
			mpp_frame_deinit(&fr);
			if (maxf && got >= maxf) goto out;
		}
		if (put_done && idle) { if (++idle_rounds > 20) break; }
		else idle_rounds = 0;
		usleep(3000);
	}
out:
	printf(">>> 解出 %d 帧\n", got);
	mpp_packet_deinit(&pkt);
	mpp_destroy(ctx);
	free(buf);
	return got > 0 ? 0 : 1;
}
