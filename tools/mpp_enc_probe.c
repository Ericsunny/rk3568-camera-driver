/*
 * mpp_enc_probe.c —— RK3568 MPP 硬编冒烟测试
 *
 * 从原始 NV12 文件循环读帧,用 VEPU 编码成 H.264,写出 .h264 基本流,
 * 打印每帧包大小、编码耗时,最后输出统计。
 *
 * 板载编译:
 *   gcc -O2 -o mpp_enc_probe mpp_enc_probe.c -lrockchip_mpp -lpthread
 * 用法:
 *   ./mpp_enc_probe <in.nv12> <w> <h> <frames> <out.h264> [bitrate_kbps]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr,
			"用法: %s <in.nv12> <w> <h> <frames> <out.h264> [bps_kbps]\n",
			argv[0]);
		return 2;
	}
	const char *inpath = argv[1];
	int w = atoi(argv[2]);
	int h = atoi(argv[3]);
	int nframes = atoi(argv[4]);
	const char *outpath = argv[5];
	int kbps = (argc > 6) ? atoi(argv[6]) : 8000;

	size_t fsize = (size_t)w * h * 3 / 2;
	FILE *fi = fopen(inpath, "rb");
	if (!fi) { perror("打开输入"); return 1; }
	FILE *fo = fopen(outpath, "wb");
	if (!fo) { perror("打开输出"); fclose(fi); return 1; }

	unsigned char *src = malloc(fsize);
	if (!src) { fprintf(stderr, "内存不足\n"); return 1; }
	if (fread(src, 1, fsize, fi) != fsize) {
		fprintf(stderr, "输入文件不足一帧(需要 %zu 字节)\n", fsize);
		return 1;
	}
	rewind(fi);
	printf(">>> 输入 %s %dx%d,每帧 %zu 字节,编 %d 帧,目标 %d kbps\n",
	       inpath, w, h, fsize, nframes, kbps);

	MppCtx ctx = NULL;
	MppApi *mpi = NULL;
	MppBufferGroup grp = NULL;
	MppBuffer frm_buf = NULL;
	MppEncCfg cfg = NULL;
	MPP_RET ret;

	if ((ret = mpp_create(&ctx, &mpi)) != MPP_OK) {
		fprintf(stderr, "mpp_create 失败 %d\n", ret);
		return 1;
	}
	if ((ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC)) != MPP_OK) {
		fprintf(stderr, "mpp_init 失败 %d\n", ret);
		return 1;
	}

	mpp_enc_cfg_init(&cfg);
	mpp_enc_cfg_set_s32(cfg, "prep:width", w);
	mpp_enc_cfg_set_s32(cfg, "prep:height", h);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", w);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", h);
	mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_MPEG);

	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_target", kbps * 1000);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", kbps * 1000 * 17 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", kbps * 1000 * 15 / 16);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", 30);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", 30);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:gop", 30);

	mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
	mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
	mpp_enc_cfg_set_s32(cfg, "h264:level", 51);
	mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
	mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
	mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

	if ((ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg)) != MPP_OK) {
		fprintf(stderr, "MPP_ENC_SET_CFG 失败 %d\n", ret);
		return 1;
	}

	mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
	if ((ret = mpp_buffer_get(grp, &frm_buf, (size_t)w * h * 3 / 2)) != MPP_OK) {
		fprintf(stderr, "mpp_buffer_get 失败 %d\n", ret);
		return 1;
	}

	long total_bytes = 0;
	int key = 0;
	double t0 = now_ms();
	for (int i = 0; i < nframes; i++) {
		if (fread(src, 1, fsize, fi) != fsize)
			rewind(fi);	/* 文件不够就循环 */

		void *ptr = mpp_buffer_get_ptr(frm_buf);
		memcpy(ptr, src, fsize);

		MppFrame frame = NULL;
		mpp_frame_init(&frame);
		mpp_frame_set_width(frame, w);
		mpp_frame_set_height(frame, h);
		mpp_frame_set_hor_stride(frame, w);
		mpp_frame_set_ver_stride(frame, h);
		mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
		mpp_frame_set_buffer(frame, frm_buf);
		mpp_frame_set_eos(frame, 0);

		ret = mpi->encode_put_frame(ctx, frame);
		if (ret != MPP_OK) {
			fprintf(stderr, "第 %d 帧 put_frame 失败 %d\n", i, ret);
			break;
		}

		MppPacket packet = NULL;
		ret = mpi->encode_get_packet(ctx, &packet);
		if (ret != MPP_OK || !packet) {
			fprintf(stderr, "第 %d 帧 get_packet 失败 %d\n", i, ret);
			mpp_frame_deinit(&frame);
			break;
		}
		size_t len = mpp_packet_get_length(packet);
		void *p = mpp_packet_get_data(packet);
		fwrite(p, 1, len, fo);
		total_bytes += len;
		if (i == 0 || i == nframes - 1)
			printf("    帧 %2d: %zu 字节\n", i, len);
		mpp_packet_deinit(&packet);
		mpp_frame_deinit(&frame);
	}
	double dt = now_ms() - t0;

	if ((ret = mpi->control(ctx, MPP_ENC_SET_CFG, NULL)) == MPP_OK) { /* no-op */ }
	mpp_buffer_put(frm_buf);
	mpp_buffer_group_put(grp);
	mpp_destroy(ctx);
	fclose(fi);
	fclose(fo);
	free(src);

	printf(">>> 完成: %d 帧, %ld 字节, 平均 %.1f kbps @30fps, 用时 %.1f ms (%.2f ms/帧)\n",
	       nframes, total_bytes, total_bytes * 8.0 * 30.0 / (nframes * 1000.0),
	       dt, dt / nframes);
	printf(">>> 输出: %s\n", outpath);
	(void)key;
	return 0;
}
