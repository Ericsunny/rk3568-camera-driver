/* Compare both actual evrec preprocessing paths and NPU output on held frames.
 * Only aggregate mismatch counts are printed; captured images are not saved.
 */
#define main evrec_application_main
#include "../evrec.c"
#undef main

static int reference_context(const char *path, rknn_context *ctx)
{
	struct stat st;
	void *model;
	FILE *f;
	int ret;
	if (stat(path, &st) || st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX)
		return -1;
	f = fopen(path, "rb");
	if (!f) return -1;
	model = malloc((size_t)st.st_size);
	if (!model || fread(model, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
		free(model); fclose(f); return -1;
	}
	fclose(f);
	ret = rknn_init(ctx, model, (uint32_t)st.st_size, 0, NULL);
	free(model);
	return ret == RKNN_SUCC ? 0 : -1;
}

static int compare_inference(rknn_context ref, const unsigned char *rgb,
			     size_t *different)
{
	rknn_input in = {0};
	rknn_output a[MAX_OUT], b[MAX_OUT];
	unsigned i;
	int have_a = 0, have_b = 0, rc = -1;
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	for (i = 0; i < g_nout; i++) a[i].index = b[i].index = i;
	in.buf = (void *)rgb;
	in.size = (uint32_t)((size_t)g_npu_w * g_npu_h * 3);
	in.type = RKNN_TENSOR_UINT8;
	in.fmt = RKNN_TENSOR_NHWC;
	if (rknn_inputs_set(ref, 1, &in) || rknn_run(ref, NULL) ||
	    rknn_outputs_get(ref, g_nout, a, NULL)) goto done;
	have_a = 1;
	if (rknn_mem_sync(g_nctx, g_npu_rgb_mem, RKNN_MEMORY_SYNC_TO_DEVICE) ||
	    rknn_run(g_nctx, NULL) || rknn_outputs_get(g_nctx, g_nout, b, NULL))
		goto done;
	have_b = 1;
	*different = 0;
	for (i = 0; i < g_nout; i++) {
		size_t j;
		const unsigned char *x = a[i].buf, *y = b[i].buf;
		if (!x || !y || a[i].size != b[i].size) goto done;
		for (j = 0; j < a[i].size; j++) if (x[j] != y[j]) (*different)++;
	}
	rc = 0;
done:
	if (have_b) rknn_outputs_release(g_nctx, g_nout, b);
	if (have_a) rknn_outputs_release(ref, g_nout, a);
	return rc;
}

int main(int argc, char **argv)
{
	rknn_context ref = 0;
	unsigned char *regular = NULL, *reference = NULL, *shared = NULL;
	size_t rgb_size = 640u * 640u * 3u, rgb_diff = 0, tensor_diff = 0;
	int frames, i = 0, rc = 1;

	if (argc != 6) {
		fprintf(stderr, "usage: %s MODEL VIDEO WIDTH HEIGHT FRAMES\n", argv[0]);
		return 2;
	}
	frames = atoi(argv[5]);
	if (frames < 1 || frames > 60 || atoi(argv[3]) < 2 || atoi(argv[4]) < 2)
		return 2;
	g_model = argv[1];
	g_npu_input_dmabuf = 1;
	if (npu_init() || reference_context(argv[1], &ref)) goto done;
	shared = g_rgb;
	regular = malloc(rgb_size);
	reference = malloc(rgb_size);
	if (!regular || !reference) goto done;
	g_input_dmabuf = 1;
	if (cap_open(argv[2], atoi(argv[3]), atoi(argv[4]), 4)) goto done;
	for (i = 0; i < frames; i++) {
		struct pollfd pollfd = { .fd = cap.fd, .events = POLLIN };
		struct v4l2_buffer b = {0};
		struct v4l2_plane pl = {0};
		size_t j, npu_different = 0;
		int srcfd;
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_DMABUF;
		b.length = 1;
		b.m.planes = &pl;
		if (poll(&pollfd, 1, 3000) <= 0 || xioctl(cap.fd, VIDIOC_DQBUF, &b) ||
		    b.index >= (unsigned)cap.nbufs || (b.flags & V4L2_BUF_FLAG_ERROR))
			goto done;
		srcfd = mpp_buffer_get_fd(cap.input[b.index]);
		if (srcfd < 0) goto done;
		g_rgb = regular;
		g_npu_input_dmabuf = 0;
		if (rga_scale(cap.map[b.index], cap.w, cap.h, cap.stride, srcfd)) goto done;
		memcpy(reference, regular, rgb_size);
		g_rgb = shared;
		g_npu_input_dmabuf = 1;
		if (rga_scale(cap.map[b.index], cap.w, cap.h, cap.stride, srcfd)) goto done;
		for (j = 0; j < rgb_size; j++) if (reference[j] != shared[j]) rgb_diff++;
		if (compare_inference(ref, reference, &npu_different)) goto done;
		tensor_diff += npu_different;
		pl.m.fd = srcfd;
		pl.length = (unsigned)cap.len[b.index];
		if (xioctl(cap.fd, VIDIOC_QBUF, &b)) goto done;
		if (rgb_diff || tensor_diff) break;
	}
	if (i == frames && !rgb_diff && !tensor_diff) rc = 0;
done:
	cap_release();
	free(reference);
	free(regular);
	if (ref) rknn_destroy(ref);
	if (g_npu_rgb_mem) rknn_destroy_mem(g_nctx, g_npu_rgb_mem);
	if (g_nctx) rknn_destroy(g_nctx);
	printf("RESULT=%s frames=%d rgb_different=%zu tensor_different=%zu\n",
	       rc ? "FAIL" : "PASS", i, rgb_diff, tensor_diff);
	return rc;
}
