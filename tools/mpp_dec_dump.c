/*
 * mpp_dec_dump.c -- 用 MPP 解码 .h264 并把每帧 Y 平面左上角裁块写成裸数据
 * (mpp_dec_probe.c 只统计不解图, OSD 验证需要把左上角抠出来看)
 *
 * 编译: gcc -O2 -o mpp_dec_dump mpp_dec_dump.c -lrockchip_mpp
 * 用法: ./mpp_dec_dump <in.h264> <out_prefix> [crop_w] [crop_h] [max]
 *
 * P7: 旧版用 max 截断时在解完第 max 帧就 goto out, 解码器 DPB 里还压着
 * 未输出的参考帧, 直接 mpp_destroy 会崩在库内的 free_dpb_mark ->
 * h264d_deinit 路径(SIGSEGV/exit 139)。新版把"停止写文件"和"停止解码"
 * 分开: 达到 max 后只停止落盘, 仍把剩余帧取完并等到 EOS, 再按正常
 * 顺序 mpp_packet_deinit -> mpp_destroy -> free, 不再跳过库的去初始化。
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
	int cw, ch, maxf;
	FILE *f;
	long sz, off = 0;
	unsigned char *buf, *p0;
	long n0;
	MppCtx ctx;
	MppApi *mpi;
	RK_U32 need_split = 1;
	MppPacket pkt = NULL, ep = NULL;
	int got = 0, idle_rounds = 0, put_done = 0, flush_done = 0;
	int eos_seen = 0, stop_dump = 0;

	if (argc < 3) {
		fprintf(stderr, "用法: %s <in.h264> <prefix> [cw] [ch] [max]\n",
			argv[0]);
		return 2;
	}
	cw = (argc > 3) ? atoi(argv[3]) : 400;
	ch = (argc > 4) ? atoi(argv[4]) : 100;
	maxf = (argc > 5) ? atoi(argv[5]) : 0;

	f = fopen(argv[1], "rb");
	if (!f) {
		perror("打开");
		return 1;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, ">>> 取 %s 长度失败\n", argv[1]);
		fclose(f);
		return 1;
	}
	buf = malloc((size_t)sz);
	if (!buf) {
		fprintf(stderr, ">>> malloc %ld 失败\n", sz);
		fclose(f);
		return 1;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, ">>> 读 %s 失败\n", argv[1]);
		fclose(f);
		free(buf);
		return 1;
	}
	fclose(f);
	printf(">>> 码流 %ld 字节\n", sz);

	/* 从第一个 SPS 开始(抓包可能起于 GOP 中间) */
	for (long i = 0; i + 5 < sz; i++) {
		if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1 &&
		    (buf[i+3] & 0x1f) == 7) { off = i; break; }
		if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 &&
		    buf[i+3] == 1 && (buf[i+4] & 0x1f) == 7) { off = i; break; }
	}
	printf(">>> 首个 SPS 偏移 %ld\n", off);
	p0 = buf + off;
	n0 = sz - off;

	if (mpp_create(&ctx, &mpi) != MPP_OK) {
		fprintf(stderr, ">>> mpp_create 失败\n");
		free(buf);
		return 1;
	}
	if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC) != MPP_OK) {
		fprintf(stderr, ">>> mpp_init 失败\n");
		mpp_destroy(ctx);
		free(buf);
		return 1;
	}
	mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &need_split);

	if (mpp_packet_init(&pkt, p0, (size_t)n0) != MPP_OK) {
		fprintf(stderr, ">>> mpp_packet_init 失败\n");
		mpp_destroy(ctx);
		free(buf);
		return 1;
	}
	mpp_packet_set_pos(pkt, p0);
	mpp_packet_set_length(pkt, (size_t)n0);

	for (int rounds = 0; rounds < 2000 && !eos_seen; rounds++) {
		if (!put_done && mpi->decode_put_packet(ctx, pkt) == MPP_OK)
			put_done = 1;
		if (put_done && !flush_done) {	/* 空包 + EOS 让解码器吐出最后一张 AU */
			if (mpp_packet_init(&ep, NULL, 0) != MPP_OK)
				break;
			mpp_packet_set_eos(ep);
			if (mpi->decode_put_packet(ctx, ep) == MPP_OK)
				flush_done = 1;
		}

		int idle = 1;
		for (;;) {
			MppFrame fr = NULL;

			if (mpi->decode_get_frame(ctx, &fr) != MPP_OK || !fr)
				break;
			idle = 0;
			if (mpp_frame_get_info_change(fr)) {
				printf(">>> 分辨率变更 %dx%d\n",
				       mpp_frame_get_width(fr),
				       mpp_frame_get_height(fr));
				mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
				mpp_frame_deinit(&fr);
				continue;
			}
			if (mpp_frame_get_eos(fr)) {
				eos_seen = 1;
				mpp_frame_deinit(&fr);
				break;
			}
			if (!stop_dump) {
				int W = (int)mpp_frame_get_width(fr);
				int H = (int)mpp_frame_get_height(fr);
				int hs = (int)mpp_frame_get_hor_stride(fr);
				MppBuffer b = mpp_frame_get_buffer(fr);

				if (hs <= 0)
					hs = W;
				if (b) {
					unsigned char *px = mpp_buffer_get_ptr(b);
					int w = cw < W ? cw : W;
					int h = ch < H ? ch : H;
					char name[512];
					FILE *o;

					snprintf(name, sizeof(name),
						 "%s_f%03d_%dx%d_stride%d.y",
						 argv[2], got, w, h, hs);
					o = fopen(name, "wb");
					if (o) {
						for (int r = 0; r < h; r++)
							fwrite(px + (size_t)r * hs, 1,
							       (size_t)w, o);
						fclose(o);
					}
					if (got == 0)
						printf(">>> 首帧 %dx%d hor_stride=%d -> %s\n",
						       W, H, hs, name);
				}
			}
			got++;
			mpp_frame_deinit(&fr);
			if (maxf && got >= maxf)
				stop_dump = 1;	/* 只停止落盘, 继续解到 EOS */
		}
		if (put_done && idle) {
			if (++idle_rounds > 50 && flush_done)
				break;
		} else {
			idle_rounds = 0;
		}
		usleep(3000);
	}

	printf(">>> 解出 %d 帧%s\n", got,
	       (maxf && got >= maxf) ? " (已达到 max, 其余帧只解码不落盘)" : "");
	fflush(stdout);

	mpp_packet_deinit(&pkt);
	if (ep)
		mpp_packet_deinit(&ep);
	/* 正常路径: EOS 已消费, DPB 已排空, 可以安全去初始化。
	 * 未收到 EOS 说明码流/解码器异常, 此时 mpp_destroy 会崩在库内
	 * free_dpb_mark(旧版 exit 139 的根因), 打印后不带崩溃退出。 */
	if (eos_seen) {
		mpp_destroy(ctx);
		free(buf);
		return got > 0 ? 0 : 1;
	}
	fprintf(stderr, ">>> 未收到 EOS(码流不完整?), 跳过 mpp_destroy 以免库内崩溃\n");
	free(buf);
	return 3;
}
