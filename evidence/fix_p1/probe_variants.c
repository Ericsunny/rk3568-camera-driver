/* probe_variants.c —— 验证 MPP EACH_IDR 模式下首帧 SPS/PPS 是否随包
 *
 * 模式:
 *   none           不调 MPP_ENC_GET_HDR_SYNC, 直接编码
 *   null           evrec 现状: MppPacket hdr=NULL; control(..., &hdr)
 *   good           正确用法: mpp_packet_init 传入用户缓冲
 *   null_then_good evrec 现状调用后, 首帧编完再正确取一次头
 *
 * 编译: gcc -O2 -o probe_variants probe_variants.c -lrockchip_mpp -lpthread
 * 用法: ./probe_variants <mode> [w h nframes gop]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

static void nal_types(const unsigned char *d, size_t n, int max, char *out, size_t outsz)
{
	size_t i = 0;
	int cnt = 0;
	out[0] = 0;
	while (i + 4 <= n && cnt < max) {
		if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) {
			unsigned t = d[i+3] & 0x1f;
			const char *nm = NULL;
			char b[8];
			if (t == 7) nm = "SPS";
			else if (t == 8) nm = "PPS";
			else if (t == 6) nm = "SEI";
			else if (t == 5) nm = "I";
			else if (t == 1) nm = "P";
			if (!nm) { snprintf(b, sizeof b, "%u", t); nm = b; }
			if (cnt) strncat(out, " ", outsz - strlen(out) - 1);
			strncat(out, nm, outsz - strlen(out) - 1);
			cnt++;
			i += 3;
		} else {
			i++;
		}
	}
}

int main(int argc, char **argv)
{
	const char *mode = (argc > 1) ? argv[1] : "none";
	int w = (argc > 2) ? atoi(argv[2]) : 640;
	int h = (argc > 3) ? atoi(argv[3]) : 480;
	int nframes = (argc > 4) ? atoi(argv[4]) : 45;
	int gop = (argc > 5) ? atoi(argv[5]) : 30;

	MppCtx ctx = NULL;
	MppApi *mpi = NULL;
	MppBufferGroup grp = NULL;
	MppBuffer fbuf = NULL;
	MppEncCfg cfg = NULL;
	MPP_RET ret;

	if ((ret = mpp_create(&ctx, &mpi)) != MPP_OK) { fprintf(stderr, "mpp_create %d\n", ret); return 1; }
	if ((ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC)) != MPP_OK) { fprintf(stderr, "mpp_init %d\n", ret); return 1; }

	mpp_enc_cfg_init(&cfg);
	mpp_enc_cfg_set_s32(cfg, "prep:width", w);
	mpp_enc_cfg_set_s32(cfg, "prep:height", h);
	mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", w);
	mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", h);
	mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
	mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_target", 4000000);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_max", 4250000);
	mpp_enc_cfg_set_s32(cfg, "rc:bps_min", 3750000);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", 30);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", 30);
	mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
	mpp_enc_cfg_set_s32(cfg, "rc:gop", gop);
	mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
	mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
	mpp_enc_cfg_set_s32(cfg, "h264:level", 51);
	if ((ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg)) != MPP_OK) { fprintf(stderr, "SET_CFG %d\n", ret); return 1; }

	MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
	mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &hm);

	if (strcmp(mode, "null") == 0) {
		MppPacket hdr = NULL;
		ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, &hdr);
		printf("[%s] control(&hdr) ret=%d hdr=%p\n", mode, ret, (void *)hdr);
	} else if (strcmp(mode, "good") == 0) {
		unsigned char *buf = malloc(4096);
		MppPacket hdr = NULL;
		mpp_packet_init(&hdr, buf, 4096);
		printf("[%s] packet_init hdr=%p len_before=%zu\n", mode, (void *)hdr, mpp_packet_get_length(hdr));
		ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hdr);
		printf("[%s] control(hdr) ret=%d len=%zu pos=%p\n", mode, ret,
		       mpp_packet_get_length(hdr), mpp_packet_get_pos(hdr));
		if (ret == MPP_OK && mpp_packet_get_length(hdr) > 0) {
			char s[256];
			nal_types((const unsigned char *)mpp_packet_get_pos(hdr),
				  mpp_packet_get_length(hdr), 12, s, sizeof s);
			printf("[%s] hdr nals: %s\n", mode, s);
		}
		mpp_packet_deinit(&hdr);
		free(buf);
	} else if (strcmp(mode, "none") != 0 && strcmp(mode, "null_then_good") != 0) {
		fprintf(stderr, "unknown mode %s\n", mode);
		return 2;
	} else if (strcmp(mode, "null_then_good") == 0) {
		MppPacket hdr = NULL;
		ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, &hdr);
		printf("[null_then_good] pre control(&hdr) ret=%d hdr=%p\n", ret, (void *)hdr);
	}

	mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
	size_t fsize = (size_t)w * h * 3 / 2;
	if ((ret = mpp_buffer_get(grp, &fbuf, fsize)) != MPP_OK) { fprintf(stderr, "buffer_get %d\n", ret); return 1; }
	memset(mpp_buffer_get_ptr(fbuf), 0x80, fsize);

	for (int i = 0; i < nframes; i++) {
		MppFrame frame = NULL;
		mpp_frame_init(&frame);
		mpp_frame_set_width(frame, w);
		mpp_frame_set_height(frame, h);
		mpp_frame_set_hor_stride(frame, w);
		mpp_frame_set_ver_stride(frame, h);
		mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
		mpp_frame_set_buffer(frame, fbuf);
		ret = mpi->encode_put_frame(ctx, frame);
		if (ret != MPP_OK) { fprintf(stderr, "put_frame %d: %d\n", i, ret); break; }

		MppPacket pkt = NULL;
		ret = mpi->encode_get_packet(ctx, &pkt);
		if (ret != MPP_OK || !pkt) { fprintf(stderr, "get_packet %d: %d\n", i, ret); mpp_frame_deinit(&frame); break; }
		size_t len = mpp_packet_get_length(pkt);
		unsigned char *d = mpp_packet_get_pos(pkt);
		unsigned flag = mpp_packet_get_flag(pkt);
		if (i < 3 || i == gop || i == gop + 1 || i == 2 * gop) {
			char s[512];
			nal_types(d, len, 16, s, sizeof s);
			int has_sps = 0, has_pps = 0;
			for (size_t k = 0; k + 3 < len; k++)
				if (d[k] == 0 && d[k+1] == 0 && d[k+2] == 1) {
					if ((d[k+3] & 0x1f) == 7) has_sps = 1;
					if ((d[k+3] & 0x1f) == 8) has_pps = 1;
				}
			printf("[%s] frame %2d len=%zu flag=0x%x sps=%d pps=%d: %s\n",
			       mode, i, len, flag, has_sps, has_pps, s);
		}
		mpp_packet_deinit(&pkt);
		mpp_frame_deinit(&frame);

		if (i == 0 && strcmp(mode, "null_then_good") == 0) {
			unsigned char *buf = malloc(4096);
			MppPacket hdr2 = NULL;
			mpp_packet_init(&hdr2, buf, 4096);
			ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hdr2);
			printf("[null_then_good] post-frame0 control(hdr) ret=%d len=%zu\n", ret, mpp_packet_get_length(hdr2));
			if (ret == MPP_OK && mpp_packet_get_length(hdr2) > 0) {
				char s[256];
				nal_types((const unsigned char *)mpp_packet_get_pos(hdr2),
					  mpp_packet_get_length(hdr2), 12, s, sizeof s);
				printf("[null_then_good] post hdr nals: %s\n", s);
			}
			mpp_packet_deinit(&hdr2);
			free(buf);
		}
	}

	mpp_buffer_put(fbuf);
	mpp_buffer_group_put(grp);
	mpp_destroy(ctx);
	return 0;
}
