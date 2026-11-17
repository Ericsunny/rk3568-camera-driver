/* Board-side: compare the actual evrec RGA paths on identical synthetic NV12. */
#define main evrec_application_main
#include "../evrec.c"
#undef main

int main(int argc, char **argv)
{
	static const unsigned shapes[][2] = {
		{1920, 1080}, {1280, 960}, {1080, 1920}, {640, 640}, {1920, 1080}
	};
	unsigned char *reference = NULL, *regular = NULL, *shared = NULL;
	size_t rgb_size = 640u * 640u * 3u;
	int rc = 1;
	unsigned k;

	if (argc != 2) return 2;
	g_model = argv[1];
	g_npu_input_dmabuf = 1;
	if (npu_init() != 0) goto done;
	shared = g_rgb;
	regular = malloc(rgb_size);
	reference = malloc(rgb_size);
	if (!regular || !reference) goto done;
	for (k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
		unsigned w = shapes[k][0], h = shapes[k][1];
		size_t n = (size_t)w * h * 3 / 2, i, different = 0;
		unsigned char *src = malloc(n);
		if (!src) goto done;
		for (i = 0; i < n; i++) src[i] = (unsigned char)((i * 37u + k * 71u) & 255u);
		g_npu_input_dmabuf = 0;
		g_rgb = regular;
		if (rga_scale(src, w, h, w, -1) != 0) { free(src); goto done; }
		memcpy(reference, regular, rgb_size);
		g_npu_input_dmabuf = 1;
		g_rgb = shared;
		if (rga_scale(src, w, h, w, -1) != 0) { free(src); goto done; }
		for (i = 0; i < rgb_size; i++) if (reference[i] != shared[i]) different++;
		free(src);
		printf("shape=%ux%u differing_bytes=%zu\n", w, h, different);
		if (different) goto done;
	}
	rc = 0;
done:
	free(reference);
	free(regular);
	g_rgb = shared;
	if (g_npu_rgb_mem) rknn_destroy_mem(g_nctx, g_npu_rgb_mem);
	if (g_nctx) rknn_destroy(g_nctx);
	printf("RESULT=%s\n", rc ? "FAIL" : "PASS");
	return rc;
}
