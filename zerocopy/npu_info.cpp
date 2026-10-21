/*
 * npu_info.cpp — 打印 rknn 模型的输入/输出张量属性(为 S4 铺路,不猜)
 *
 * 编译:g++ -O2 -o npu_info npu_info.cpp -I/root/npu/3rdparty/rknpu2/include -lrknnrt
 * 用法:./npu_info /path/to/model.rknn
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "rknn_api.h"

static const char *type_str(rknn_tensor_type t)
{
	switch (t) {
	case RKNN_TENSOR_FLOAT32: return "FP32";
	case RKNN_TENSOR_FLOAT16: return "FP16";
	case RKNN_TENSOR_INT8:    return "INT8";
	case RKNN_TENSOR_UINT8:   return "UINT8";
	case RKNN_TENSOR_INT16:   return "INT16";
	default: return "?";
	}
}
static const char *fmt_str(rknn_tensor_format f)
{
	switch (f) {
	case RKNN_TENSOR_NCHW: return "NCHW";
	case RKNN_TENSOR_NHWC: return "NHWC";
	case RKNN_TENSOR_NC1HWC2: return "NC1HWC2";
	default: return "?";
	}
}
static const char *qnt_str(rknn_tensor_qnt_type q)
{
	switch (q) {
	case RKNN_TENSOR_QNT_NONE: return "NONE";
	case RKNN_TENSOR_QNT_DFP: return "DFP";
	case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC: return "AFFINE_ASYMMETRIC";
	default: return "?";
	}
}

static void dump(const char *tag, rknn_tensor_attr *a)
{
	printf("%s: name=%s n_dims=%u dims=[", tag, a->name, a->n_dims);
	for (unsigned i = 0; i < a->n_dims; i++) printf("%s%u", i ? "," : "", a->dims[i]);
	printf("]\n");
	printf("      fmt=%s type=%s qnt=%s zp=%d scale=%f\n",
	       fmt_str(a->fmt), type_str(a->type), qnt_str(a->qnt_type), a->zp, a->scale);
	printf("      size=%u size_with_stride=%u w_stride=%u h_stride=%u\n",
	       a->size, a->size_with_stride, a->w_stride, a->h_stride);
	printf("      n_elems=%u fl=%d pass_through=%u\n",
	       a->n_elems, a->fl, a->pass_through);
}

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1]
		: "/root/npu/examples/yolov8/model/yolov8n_rk3568_i8.rknn";
	FILE *f = fopen(path, "rb");
	if (!f) { perror("open model"); return 1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	void *model = malloc(sz);
	if (fread(model, 1, sz, f) != (size_t)sz) { printf("read fail\n"); return 1; }
	fclose(f);
	printf("model: %s (%ld bytes)\n\n", path, sz);

	rknn_context ctx = 0;
	int ret = rknn_init(&ctx, model, sz, 0, NULL);
	if (ret < 0) { printf("rknn_init failed: %d\n", ret); return 1; }

	rknn_input_output_num io_num;
	ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof io_num);
	if (ret < 0) { printf("query io_num failed: %d\n", ret); return 1; }
	printf("input num=%u  output num=%u\n\n", io_num.n_input, io_num.n_output);

	for (unsigned i = 0; i < io_num.n_input; i++) {
		rknn_tensor_attr a; memset(&a, 0, sizeof a);
		a.index = i;
		rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof a);
		dump("INPUT ", &a);
	}
	printf("\n");
	for (unsigned i = 0; i < io_num.n_output; i++) {
		rknn_tensor_attr a; memset(&a, 0, sizeof a);
		a.index = i;
		rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof a);
		dump("OUTPUT", &a);
	}
	rknn_destroy(ctx);
	free(model);
	return 0;
}
