/* One producer, one inference worker. Fixed input slots and bounded results. */
#ifndef EVREC_PIPELINE_H
#define EVREC_PIPELINE_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#define EP_SLOTS 2
#define EP_RESULTS 8
enum ep_state { EP_FREE, EP_WRITING, EP_READY, EP_RUNNING };
struct ep_job {
    uint32_t epoch, seq, driver_seq;
    uint64_t dq_ns;
    double media_ms;
};
struct ep_detection {
    int hit, nd;
    float score, maxscore;
    double mean;
    uint64_t input_hash, output_hash;
};
struct ep_result {
    struct ep_job job;
    struct ep_detection detection;
    uint64_t started_ns, completed_ns;
};
struct ep_stats { uint64_t submitted, replaced, completed, cancelled; int failed; };
typedef int (*ep_run_fn)(int, const struct ep_job *, struct ep_detection *, void *);
struct ep_queue {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t worker;
    enum ep_state state[EP_SLOTS];
    struct ep_job job[EP_SLOTS];
    struct ep_result result[EP_RESULTS];
    unsigned head, count, next;
    int pending, stopping, joined;
    ep_run_fn run;
    void *context;
    struct ep_stats stats;
};
static inline uint64_t ep_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static inline uint64_t ep_hash(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n--) { h ^= *p++; h *= UINT64_C(1099511628211); }
    return h;
}
static void *ep_worker(void *arg)
{
    struct ep_queue *q = arg;
    for (;;) {
        struct ep_result r;
        int slot, rc;
        memset(&r, 0, sizeof(r));
        pthread_mutex_lock(&q->lock);
        while (q->pending < 0 && !q->stopping)
            pthread_cond_wait(&q->wake, &q->lock);
        if (q->pending < 0 || q->stats.failed) {
            pthread_mutex_unlock(&q->lock);
            return NULL;
        }
        slot = q->pending; q->pending = -1;
        q->state[slot] = EP_RUNNING; r.job = q->job[slot];
        pthread_mutex_unlock(&q->lock);
        r.started_ns = ep_now();
        rc = q->run(slot, &r.job, &r.detection, q->context);
        r.completed_ns = ep_now();
        pthread_mutex_lock(&q->lock);
        q->state[slot] = EP_FREE;
        if (rc || q->count == EP_RESULTS) {
            q->stats.failed = 1; q->stopping = 1;
            if (q->pending >= 0) {
                q->state[q->pending] = EP_FREE; q->pending = -1;
                q->stats.cancelled++;
            }
        } else {
            q->result[(q->head + q->count) % EP_RESULTS] = r;
            q->count++; q->stats.completed++;
        }
        pthread_cond_broadcast(&q->wake);
        pthread_mutex_unlock(&q->lock);
    }
}
static inline int ep_open(struct ep_queue *q, ep_run_fn run, void *context)
{
    int rc;
    memset(q, 0, sizeof(*q)); q->pending = -1; q->run = run; q->context = context;
    rc = pthread_mutex_init(&q->lock, NULL);
    if (rc) return -1;
    rc = pthread_cond_init(&q->wake, NULL);
    if (rc) { pthread_mutex_destroy(&q->lock); return -1; }
    rc = pthread_create(&q->worker, NULL, ep_worker, q);
    if (rc) { pthread_cond_destroy(&q->wake); pthread_mutex_destroy(&q->lock); return -1; }
    return 0;
}
static inline int ep_reserve(struct ep_queue *q)
{
    unsigned i;
    int slot = -1;
    pthread_mutex_lock(&q->lock);
    if (q->stopping || q->stats.failed) goto done;
    for (i = 0; i < EP_SLOTS; i++) {
        unsigned n = (q->next + i) % EP_SLOTS;
        if (q->state[n] == EP_FREE) { slot = (int)n; break; }
    }
    if (slot < 0 && q->pending >= 0) {
        slot = q->pending; q->pending = -1; q->stats.replaced++;
    }
    if (slot >= 0) { q->state[slot] = EP_WRITING; q->next = (slot + 1) % EP_SLOTS; }
done:
    pthread_mutex_unlock(&q->lock);
    return slot;
}
static inline int ep_submit(struct ep_queue *q, int slot, const struct ep_job *job)
{
    int rc = -1;
    pthread_mutex_lock(&q->lock);
    if (slot < 0 || slot >= EP_SLOTS || q->state[slot] != EP_WRITING ||
        q->stopping || q->stats.failed) goto done;
    if (q->pending >= 0) {
        q->state[q->pending] = EP_FREE; q->stats.replaced++;
    }
    q->job[slot] = *job; q->state[slot] = EP_READY; q->pending = slot;
    q->stats.submitted++; rc = 0; pthread_cond_signal(&q->wake);
done:
    pthread_mutex_unlock(&q->lock);
    return rc;
}
static inline int ep_pop(struct ep_queue *q, struct ep_result *result)
{
    int rc;
    pthread_mutex_lock(&q->lock);
    rc = q->stats.failed ? -1 : 0;
    if (q->count) {
        *result = q->result[q->head]; q->head = (q->head + 1) % EP_RESULTS;
        q->count--; rc = 1;
    }
    pthread_mutex_unlock(&q->lock);
    return rc;
}
static inline struct ep_stats ep_snapshot(struct ep_queue *q)
{
    struct ep_stats s;
    pthread_mutex_lock(&q->lock); s = q->stats; pthread_mutex_unlock(&q->lock);
    return s;
}
/* Timeout leaves the live queue intact. The caller must not free its memory. */
static inline int ep_join(struct ep_queue *q, unsigned timeout_ms)
{
    struct timespec end;
    int rc;
    if (q->joined) return 0;
    pthread_mutex_lock(&q->lock); q->stopping = 1;
    pthread_cond_broadcast(&q->wake); pthread_mutex_unlock(&q->lock);
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += timeout_ms / 1000;
    end.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (end.tv_nsec >= 1000000000) { end.tv_sec++; end.tv_nsec -= 1000000000; }
    rc = pthread_timedjoin_np(q->worker, NULL, &end);
    if (rc) { errno = rc; return -1; }
    q->joined = 1;
    return 0;
}
static inline void ep_destroy(struct ep_queue *q)
{
    pthread_cond_destroy(&q->wake); pthread_mutex_destroy(&q->lock);
}
#endif
