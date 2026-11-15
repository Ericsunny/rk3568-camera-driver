/* Bounded, single-producer event writer. Payloads are owned by the queue. */
#ifndef EVREC_WRITER_H
#define EVREC_WRITER_H

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct evw_packet {
	struct evw_packet *next;
	size_t len;
	unsigned seq, pkt;
	double ts_ms, cap_ms;
	int key;
	unsigned char data[];
};

struct evw_queue {
	pthread_mutex_t lock;
	pthread_cond_t ready;
	pthread_t thread;
	struct evw_packet *head, *tail;
	size_t bytes, count, max_bytes, max_count;
	int started, closing, failed;
	int (*consume)(const struct evw_packet *);
	int (*finish)(void);
};

#define EVW_QUEUE_INITIALIZER { .lock = PTHREAD_MUTEX_INITIALIZER, \
	.ready = PTHREAD_COND_INITIALIZER }

static void *evw_queue_worker(void *arg)
{
	struct evw_queue *q = arg;
	for (;;) {
		struct evw_packet *p;
		int failed;
		pthread_mutex_lock(&q->lock);
		while (!q->head && !q->closing)
			pthread_cond_wait(&q->ready, &q->lock);
		if (!q->head && q->closing) {
			pthread_mutex_unlock(&q->lock);
			break;
		}
		p = q->head;
		q->head = p->next;
		if (!q->head)
			q->tail = NULL;
		failed = q->failed;
		pthread_mutex_unlock(&q->lock);
		if (!failed && q->consume(p) != 0) {
			pthread_mutex_lock(&q->lock);
			q->failed = 1;
			pthread_mutex_unlock(&q->lock);
		}
		pthread_mutex_lock(&q->lock);
		q->bytes -= sizeof(*p) + p->len;
		q->count--;
		pthread_mutex_unlock(&q->lock);
		free(p);
	}
	if (q->finish && q->finish() != 0) {
		pthread_mutex_lock(&q->lock);
		q->failed = 1;
		pthread_mutex_unlock(&q->lock);
	}
	return NULL;
}

static int evw_queue_start(struct evw_queue *q, size_t max_bytes, size_t max_count,
			   int (*consume)(const struct evw_packet *), int (*finish)(void))
{
	int rc;
	if (!max_bytes || !max_count || !consume || q->started)
		return -1;
	q->head = q->tail = NULL;
	q->bytes = q->count = 0;
	q->max_bytes = max_bytes;
	q->max_count = max_count;
	q->closing = q->failed = 0;
	q->consume = consume;
	q->finish = finish;
	rc = pthread_create(&q->thread, NULL, evw_queue_worker, q);
	if (rc)
		return -1;
	q->started = 1;
	return 0;
}

/* Never waits for space. A full queue fails the whole event; no silent drop. */
static int evw_queue_push(struct evw_queue *q, const unsigned char *data, size_t len,
			  unsigned seq, unsigned pkt, double ts_ms, double cap_ms, int key)
{
	struct evw_packet *p;
	size_t cost;
	if (!q->started || !data || !len || len > SIZE_MAX - sizeof(*p))
		return -1;
	cost = sizeof(*p) + len;
	if (cost > q->max_bytes)
		return -1;
	/* Reserve before allocation: queued, in-flight and pending copies all count
	 * against the same bound. There is exactly one producer. */
	pthread_mutex_lock(&q->lock);
	if (q->failed || q->closing || q->count >= q->max_count ||
	    q->bytes > q->max_bytes - cost) {
		q->failed = 1;
		pthread_mutex_unlock(&q->lock);
		return -1;
	}
	q->bytes += cost;
	q->count++;
	pthread_mutex_unlock(&q->lock);
	p = malloc(cost);
	if (!p) {
		pthread_mutex_lock(&q->lock);
		q->bytes -= cost;
		q->count--;
		q->failed = 1;
		pthread_mutex_unlock(&q->lock);
		return -1;
	}
	p->next = NULL; p->len = len; p->seq = seq; p->pkt = pkt;
	p->ts_ms = ts_ms; p->cap_ms = cap_ms; p->key = key;
	memcpy(p->data, data, len);
	pthread_mutex_lock(&q->lock);
	if (q->failed || q->closing) {
		q->bytes -= cost;
		q->count--;
		q->failed = 1;
		pthread_mutex_unlock(&q->lock);
		free(p);
		return -1;
	}
	if (q->tail)
		q->tail->next = p;
	else
		q->head = p;
	q->tail = p;
	pthread_cond_signal(&q->ready);
	pthread_mutex_unlock(&q->lock);
	return 0;
}

static int evw_queue_failed(struct evw_queue *q)
{
	int failed;
	if (!q->started)
		return 0;
	pthread_mutex_lock(&q->lock);
	failed = q->failed;
	pthread_mutex_unlock(&q->lock);
	return failed;
}

/* Drain and join; a permanently blocked filesystem may delay this join. */
static int evw_queue_stop(struct evw_queue *q)
{
	int rc, failed;
	if (!q->started)
		return 0;
	pthread_mutex_lock(&q->lock);
	q->closing = 1;
	pthread_cond_signal(&q->ready);
	pthread_mutex_unlock(&q->lock);
	rc = pthread_join(q->thread, NULL);
	if (rc)
		return -1;
	failed = q->failed;
	q->started = 0;
	return failed ? -1 : 0;
}
#endif
