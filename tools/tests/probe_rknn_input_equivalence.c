/* Compare ordinary UINT8 RGB input with a bound RKNN input allocation.
 * Synthetic data only; no camera, recording, network, or service changes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rknn_api.h>

#define MAX_OUTPUTS 32

static int load_model(const char *path, void **data, uint32_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 ||
	    (unsigned long)n > UINT32_MAX || fseek(f, 0, SEEK_SET)) {
		if (f) fclose(f);
		return -1;
	}
	*data = malloc((size_t)n);
	if (!*data || fread(*data, 1, (size_t)n, f) != (size_t)n) {
		free(*data);
		*data = NULL;
		fclose(f);
		return -1;
	}
	fclose(f);
	*size = (uint32_t)n;
	return 0;
}

static void pattern(unsigned char *p, size_t n, int which)
{
	uint32_t state = 0x12345678u;
	size_t i;
	for (i = 0; i < n; i++) {
		state = state * 1664525u + 1013904223u;
		p[i] = which == 0 ? 0 : which == 1 ? (unsigned char)(i % 256) :
		       (unsigned char)(state >> 24);
	}
}

static int compare_outputs(rknn_output *a, rknn_output *b, unsigned count,
			   size_t *different, unsigned *maxdiff)
{
	unsigned i;
	*different = 0;
	*maxdiff = 0;
	for (i = 0; i < count; i++) {
		size_t j;
		const int8_t *x = a[i].buf, *y = b[i].buf;
		if (!x || !y || a[i].size != b[i].size)
			return -1;
		for (j = 0; j < a[i].size; j++) {
			int delta = (int)x[j] - (int)y[j];
			unsigned absdelta = (unsigned)(delta < 0 ? -delta : delta);
			if (absdelta) (*different)++;
			if (absdelta > *maxdiff) *maxdiff = absdelta;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	void *model = NULL;
	uint32_t model_size = 0;
	rknn_context ref = 0, candidate = 0;
	rknn_tensor_mem *mem = NULL;
	rknn_tensor_attr attr;
	rknn_input_output_num io;
	unsigned char *rgb = NULL;
	size_t input_size, different;
	unsigned maxdiff, i;
	int mode, sample, result = 1;

	if (argc != 3 || (strcmp(argv[2], "convert") && strcmp(argv[2], "signed"))) {
		fprintf(stderr, "usage: %s MODEL.rknn convert|signed\n", argv[0]);
		return 2;
	}
	mode = !strcmp(argv[2], "signed");
	if (load_model(argv[1], &model, &model_size) ||
	    rknn_init(&ref, model, model_size, 0, NULL) != RKNN_SUCC ||
	    rknn_init(&candidate, model, model_size, 0, NULL) != RKNN_SUCC)
		goto done;
	memset(&attr, 0, sizeof(attr));
	memset(&io, 0, sizeof(io));
	if (rknn_query(candidate, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &attr, sizeof(attr)) ||
	    rknn_query(ref, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) ||
	    attr.n_dims != 4 || attr.dims[0] != 1 || attr.dims[3] != 3 ||
	    attr.fmt != RKNN_TENSOR_NHWC || attr.type != RKNN_TENSOR_INT8 ||
	    attr.zp != -128 || io.n_input != 1 || !io.n_output || io.n_output > MAX_OUTPUTS)
		goto done;
	input_size = (size_t)attr.dims[1] * attr.dims[2] * attr.dims[3];
	if (attr.size_with_stride != input_size || input_size > UINT32_MAX)
		goto done;
	rgb = malloc(input_size);
	mem = rknn_create_mem(candidate, (uint32_t)input_size);
	if (!rgb || !mem || !mem->virt_addr || mem->size < input_size)
		goto done;
	attr.pass_through = mode ? 1 : 0;
	if (!mode) attr.type = RKNN_TENSOR_UINT8;
	if (rknn_set_io_mem(candidate, mem, &attr) != RKNN_SUCC)
		goto done;
	printf("mode=%s input_bytes=%zu outputs=%u\n", argv[2], input_size, io.n_output);
	result = 0;
	for (sample = 0; sample < 3; sample++) {
		rknn_input in;
		rknn_output a[MAX_OUTPUTS], b[MAX_OUTPUTS];
		int have_a = 0, have_b = 0, err = 0;
		size_t j;
		pattern(rgb, input_size, sample);
		memset(&in, 0, sizeof(in));
		in.buf = rgb;
		in.size = (uint32_t)input_size;
		in.type = RKNN_TENSOR_UINT8;
		in.fmt = RKNN_TENSOR_NHWC;
		memset(a, 0, sizeof(a));
		memset(b, 0, sizeof(b));
		for (i = 0; i < io.n_output; i++) a[i].index = b[i].index = i;
		if (rknn_inputs_set(ref, 1, &in) || rknn_run(ref, NULL) ||
		    rknn_outputs_get(ref, io.n_output, a, NULL)) err = 1;
		else have_a = 1;
		if (mode) {
			for (j = 0; j < input_size; j++)
				((unsigned char *)mem->virt_addr)[j] = rgb[j] ^ 0x80u;
		} else {
			memcpy(mem->virt_addr, rgb, input_size);
		}
		if (!err && (rknn_mem_sync(candidate, mem, RKNN_MEMORY_SYNC_TO_DEVICE) ||
		             rknn_run(candidate, NULL) ||
		             rknn_outputs_get(candidate, io.n_output, b, NULL))) err = 1;
		else if (!err) have_b = 1;
		if (!err && compare_outputs(a, b, io.n_output, &different, &maxdiff)) err = 1;
		if (!err) {
			size_t changed = 0;
			if (rknn_mem_sync(candidate, mem, RKNN_MEMORY_SYNC_FROM_DEVICE))
				err = 1;
			for (j = 0; !err && j < input_size; j++) {
				unsigned char expected = mode ? (unsigned char)(rgb[j] ^ 0x80u) : rgb[j];
				if (((unsigned char *)mem->virt_addr)[j] != expected) changed++;
			}
			printf("sample=%d different=%zu max_abs=%u\n", sample, different, maxdiff);
			printf("sample=%d input_changed=%zu\n", sample, changed);
			if (different || changed) err = 1;
		}
		if (have_b) rknn_outputs_release(candidate, io.n_output, b);
		if (have_a) rknn_outputs_release(ref, io.n_output, a);
		if (err) {
			fprintf(stderr, "sample=%d comparison failed\n", sample);
			result = 1;
		}
	}
done:
	if (result) fprintf(stderr, "contract/equivalence failed for %s\n", argv[2]);
	if (mem) rknn_destroy_mem(candidate, mem);
	if (candidate) rknn_destroy(candidate);
	if (ref) rknn_destroy(ref);
	free(rgb);
	free(model);
	return result;
}
