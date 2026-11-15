/* Inspect the ordinary and native input contracts of one RKNN model.
 * This is a board-side diagnostic; it does not open a camera or change services.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <rknn_api.h>

static int show_attr(rknn_context ctx, int query, const char *label)
{
	rknn_tensor_attr a;
	int ret;
	unsigned i;

	memset(&a, 0, sizeof(a));
	ret = rknn_query(ctx, query, &a, sizeof(a));
	if (ret != RKNN_SUCC) {
		printf("%s query=%d\n", label, ret);
		return ret;
	}
	printf("%s dims=", label);
	for (i = 0; i < a.n_dims && i < RKNN_MAX_DIMS; i++)
		printf("%s%u", i ? "x" : "", a.dims[i]);
	printf(" fmt=%d type=%d qnt=%d zp=%d scale=%.9g size=%u stride_size=%u w_stride=%u h_stride=%u\n",
	       a.fmt, a.type, a.qnt_type, a.zp, a.scale, a.size,
	       a.size_with_stride, a.w_stride, a.h_stride);
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f;
	long size;
	void *model;
	rknn_context ctx = 0;
	rknn_sdk_version version;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s MODEL.rknn\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
	    (unsigned long)size > UINT32_MAX || fseek(f, 0, SEEK_SET) != 0) {
		if (f) fclose(f);
		fprintf(stderr, "cannot read model\n");
		return 1;
	}
	model = malloc((size_t)size);
	if (!model || fread(model, 1, (size_t)size, f) != (size_t)size) {
		free(model);
		fclose(f);
		fprintf(stderr, "short model read\n");
		return 1;
	}
	fclose(f);
	ret = rknn_init(&ctx, model, (uint32_t)size, 0, NULL);
	free(model);
	if (ret != RKNN_SUCC) {
		fprintf(stderr, "rknn_init=%d\n", ret);
		return 1;
	}
	memset(&version, 0, sizeof(version));
	ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
	if (ret == RKNN_SUCC)
		printf("sdk=%s driver=%s\n", version.api_version, version.drv_version);
	ret = show_attr(ctx, RKNN_QUERY_INPUT_ATTR, "logical");
	if (show_attr(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, "native") != 0)
		ret = -1;
	if (show_attr(ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, "native_nhwc") != 0)
		ret = -1;
	rknn_destroy(ctx);
	return ret == 0 ? 0 : 1;
}
