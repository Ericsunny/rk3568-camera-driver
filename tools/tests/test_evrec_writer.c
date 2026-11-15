/* Host-only tests of the actual bounded writer queue. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fail_create, fail_join;
static int create_test(pthread_t *t, const pthread_attr_t *a,
		       void *(*fn)(void *), void *arg)
{
	if (fail_create) return EAGAIN;
	return pthread_create(t, a, fn, arg);
}
static int join_test(pthread_t thread, void **result)
{
	if (fail_join) return EINVAL;
	return pthread_join(thread, result);
}
#define pthread_create create_test
#define pthread_join join_test
#include "../evrec_writer.h"
#undef pthread_create
#undef pthread_join

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate, entered, consume_error, finish_error, finish_count;
static unsigned seen[16], seen_count;
static char contents[16];
static atomic_int joined;

static void check(int ok, const char *msg)
{
	if (!ok) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}
static int consume(const struct evw_packet *p)
{
	pthread_mutex_lock(&gate_lock);
	entered = 1;
	pthread_cond_broadcast(&gate_cond);
	while (!gate) pthread_cond_wait(&gate_cond, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
	seen[seen_count] = p->seq;
	contents[seen_count++] = (char)p->data[0];
	return consume_error ? -1 : 0;
}
static int finish(void) { finish_count++; return finish_error ? -1 : 0; }
static void wait_entered(void)
{
	pthread_mutex_lock(&gate_lock);
	while (!entered) pthread_cond_wait(&gate_cond, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}
static void release_gate(void)
{
	pthread_mutex_lock(&gate_lock);
	gate = 1;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
}
static void reset_test(void)
{
	gate = entered = consume_error = finish_error = finish_count = 0;
	seen_count = 0;
	atomic_store(&joined, 0);
}
static void *stop_thread(void *arg)
{
	int *result = arg;
	extern struct evw_queue test_queue;
	*result = evw_queue_stop(&test_queue);
	atomic_store(&joined, 1);
	return NULL;
}
struct evw_queue test_queue = EVW_QUEUE_INITIALIZER;

int main(void)
{
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 30000000 };
	pthread_t stopper;
	int result = -99;
	unsigned char source[] = "a";

	fail_create = 1;
	check(evw_queue_start(&test_queue, 1024, 4, consume, finish) != 0,
	      "pthread_create failure is reported");
	check(!test_queue.started, "failed creation does not require join");
	fail_create = 0;
	reset_test();
	check(!evw_queue_start(&test_queue, 1024, 4, consume, finish), "start");
	check(!evw_queue_push(&test_queue, source, 1, 1, 0, 0, 0, 0), "push first");
	wait_entered();
	source[0] = 'b';
	check(!evw_queue_push(&test_queue, source, 1, 2, 0, 0, 0, 0), "push second");
	source[0] = 'z';
	check(!pthread_create(&stopper, NULL, stop_thread, &result), "stop thread");
	nanosleep(&delay, NULL);
	check(!atomic_load(&joined), "stop waits for the in-flight write");
	release_gate();
	check(!pthread_join(stopper, NULL) && !result, "drain and stop succeed");
	check(seen_count == 2 && seen[0] == 1 && seen[1] == 2 &&
	      contents[0] == 'a' && contents[1] == 'b' && finish_count == 1,
	      "FIFO and private copies survive producer reuse");
	check(!test_queue.bytes && !test_queue.count, "all payload ownership released");

	reset_test();
	check(!evw_queue_start(&test_queue, 1024, 1, consume, finish), "restart");
	check(!evw_queue_push(&test_queue, source, 1, 1, 0, 0, 0, 0), "fill");
	wait_entered();
	check(evw_queue_push(&test_queue, source, 1, 2, 0, 0, 0, 0) != 0,
	      "in-flight packet counts against queue capacity");
	check(evw_queue_failed(&test_queue), "overflow is signalled");
	release_gate();
	check(evw_queue_stop(&test_queue) != 0, "overflow fails event");
	check(!test_queue.bytes && !test_queue.count, "overflow frees all packets");
	reset_test();
	check(!evw_queue_start(&test_queue, sizeof(struct evw_packet) + 1, 4,
	                       consume, finish), "byte limit start");
	check(!evw_queue_push(&test_queue, source, 1, 1, 0, 0, 0, 0), "byte limit fill");
	wait_entered();
	check(evw_queue_push(&test_queue, source, 1, 2, 0, 0, 0, 0) != 0,
	      "in-flight packet counts against byte limit");
	release_gate();
	check(evw_queue_stop(&test_queue) != 0 && !test_queue.bytes && !test_queue.count,
	      "byte overflow fails and releases packets");

	reset_test(); gate = 1; consume_error = 1;
	check(!evw_queue_start(&test_queue, 1024, 4, consume, finish), "error start");
	check(!evw_queue_push(&test_queue, source, 1, 1, 0, 0, 0, 0), "error push");
	for (int i = 0; i < 100 && !evw_queue_failed(&test_queue); i++) nanosleep(&delay, NULL);
	check(evw_queue_failed(&test_queue), "worker failure reaches producer");
	check(evw_queue_stop(&test_queue) != 0, "worker failure reaches stop");

	reset_test(); gate = 1; finish_error = 1;
	check(!evw_queue_start(&test_queue, 1024, 4, consume, finish), "finish start");
	check(evw_queue_stop(&test_queue) != 0, "flush failure reaches stop");
	reset_test(); gate = 1;
	check(!evw_queue_start(&test_queue, 1024, 4, consume, finish), "join start");
	fail_join = 1;
	check(evw_queue_stop(&test_queue) != 0 && test_queue.started,
	      "failed join retains worker ownership");
	fail_join = 0;
	check(!evw_queue_stop(&test_queue), "retry join reclaims worker");
	puts("event writer: creation, FIFO, lifetime, overflow, errors and drain PASS");
	return 0;
}
