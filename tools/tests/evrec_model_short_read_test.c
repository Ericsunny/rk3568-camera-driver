/* Exercise actual npu_init short-read cleanup without an RKNN runtime. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <rknn_api.h>

static void *test_malloc(size_t);
static void test_free(void *);
static size_t test_fread(void *, size_t, size_t, FILE *);
#define malloc test_malloc
#define free test_free
#define fread test_fread
#define main evrec_original_main
#include "../evrec.c"
#undef main
#undef malloc
#undef free
#undef fread

static void *model_allocation;
static int model_freed;
static void *test_malloc(size_t n) { model_allocation = malloc(n); return model_allocation; }
static void test_free(void *p) { if (p == model_allocation) model_freed++; free(p); }
static size_t test_fread(void *p, size_t size, size_t count, FILE *f)
{ (void)p; (void)size; (void)count; (void)f; return 0; }

/* The injected short read returns before any of these symbols are called. */
int rknn_init(rknn_context *c, void *m, uint32_t s, uint32_t f, rknn_init_extend *e)
{ (void)c; (void)m; (void)s; (void)f; (void)e; abort(); }
int rknn_query(rknn_context c, rknn_query_cmd q, void *p, uint32_t s)
{ (void)c; (void)q; (void)p; (void)s; abort(); }
rknn_tensor_mem *rknn_create_mem(rknn_context c, uint32_t s)
{ (void)c; (void)s; abort(); }

int main(void)
{
	char path[] = "/tmp/evrec-model-test-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0 || write(fd, "model", 5) != 5 || close(fd) != 0)
		return 1;
	g_model = path;
	if (npu_init() == 0 || !model_allocation || model_freed != 1)
		return 1;
	if (unlink(path) != 0)
		return 1;
	puts("evrec model short read releases allocation: PASS");
	return 0;
}
