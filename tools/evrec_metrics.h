/* Optional bounded frame observations. No per-frame filesystem I/O. */
#ifndef EVREC_METRICS_H
#define EVREC_METRICS_H
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define EVM_CAPACITY 4096u
#define EVM_COLUMNS "epoch,frame_seq,driver_sequence,buffer_index,buffer_flags,driver_timestamp_ns,dq_mono_ns,encode_done_mono_ns,input_return_mono_ns,qbuf_done_mono_ns,input_dmabuf,completed\n"

struct evm_row {
    uint64_t driver_ns, dq_ns, encode_ns, returned_ns, qbuf_ns;
    uint32_t epoch, frame_seq, driver_seq, index, flags;
    unsigned dmabuf, completed;
};
struct evm_context {
    FILE *csv, *meta;
    struct evm_row *rows;
    uint64_t seen, completed_total;
    size_t stored;
    unsigned epoch, enabled;
};

static inline uint64_t evm_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return 0; /* An absent clock value is rejected by the analyzer. */
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static inline FILE *evm_exclusive(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    FILE *f;
    if (fd < 0)
        return NULL;
    f = fdopen(fd, "w");
    if (!f)
        close(fd);
    return f;
}

static inline int evm_open(struct evm_context *m, const char *path)
{
    char *meta;
    int saved;
    if (!path || !*path) { errno = EINVAL; return -1; }
    meta = malloc(strlen(path) + sizeof(".meta"));
    m->rows = calloc(EVM_CAPACITY, sizeof(*m->rows));
    if (!meta || !m->rows) { errno = ENOMEM; goto fail; }
    sprintf(meta, "%s.meta", path); /* JSON content, not an event .json name. */
    m->csv = evm_exclusive(path);
    if (!m->csv)
        goto fail;
    m->meta = evm_exclusive(meta);
    if (!m->meta || fputs(EVM_COLUMNS, m->csv) == EOF || fflush(m->csv))
        goto fail;
    free(meta);
    m->enabled = 1;
    return 0;
fail:
    saved = errno;
    if (m->csv) fclose(m->csv);
    if (m->meta) fclose(m->meta);
    free(m->rows); free(meta);
    memset(m, 0, sizeof(*m));
    errno = saved;
    return -1;
}

static inline void evm_new_epoch(struct evm_context *m)
{
    if (m->enabled)
        ++m->epoch;
}

static inline struct evm_row *evm_begin(struct evm_context *m, unsigned frame_seq,
        unsigned driver_seq, unsigned index, unsigned flags, uint64_t driver_ns,
        int dmabuf)
{
    struct evm_row *r;
    if (!m->enabled)
        return NULL;
    ++m->seen;
    if (m->stored == EVM_CAPACITY)
        return NULL;
    r = &m->rows[m->stored++];
    r->dq_ns = evm_now_ns();
    r->epoch = m->epoch;
    r->frame_seq = frame_seq;
    r->driver_seq = driver_seq;
    r->index = index;
    r->flags = flags;
    r->driver_ns = driver_ns;
    r->dmabuf = !!dmabuf;
    return r;
}

static inline void evm_encoded(struct evm_row *r)
{
    if (r) r->encode_ns = evm_now_ns();
}
static inline void evm_input_returned(struct evm_row *r)
{
    if (r) r->returned_ns = evm_now_ns();
}
static inline void evm_qbuf(struct evm_row *r)
{
    if (r) r->qbuf_ns = evm_now_ns();
}
static inline void evm_completed(struct evm_context *m, struct evm_row *r)
{
    ++m->completed_total;
    if (r) r->completed = 1;
}

static inline int evm_finish(struct evm_context *m)
{
    size_t i;
    int failed = 0;
    if (!m->csv && !m->meta)
        return 0;
    for (i = 0; i < m->stored; ++i) {
        const struct evm_row *r = &m->rows[i];
        if (fprintf(m->csv, "%u,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%u,%u\n", r->epoch, r->frame_seq,
                r->driver_seq, r->index, r->flags, r->driver_ns, r->dq_ns,
                r->encode_ns, r->returned_ns, r->qbuf_ns, r->dmabuf, r->completed) < 0) {
            failed = 1;
            break;
        }
    }
    if (fclose(m->csv)) failed = 1;
    m->csv = NULL;
    if (fprintf(m->meta, "{\"schema\":1,\"clock\":\"CLOCK_MONOTONIC\","
                "\"capacity\":%u,\"seen\":%" PRIu64 ",\"stored\":%zu,"
                "\"completed_total\":%" PRIu64 ",\"epochs\":%u,"
                "\"truncated\":%s,\"write_error\":%s}\n", EVM_CAPACITY,
                m->seen, m->stored, m->completed_total, m->epoch,
                m->seen > m->stored ? "true" : "false", failed ? "true" : "false") < 0)
        failed = 1;
    if (fclose(m->meta)) failed = 1;
    m->meta = NULL;
    free(m->rows);
    m->rows = NULL;
    return failed ? -1 : 0;
}

/* Called at the same final cleanup point with observations on and off. */
static inline void evm_report(const struct evm_context *m)
{
    struct rusage usage;
    if (!getrusage(RUSAGE_SELF, &usage))
        printf("frame-metrics: enabled=%u seen=%" PRIu64 " stored=%zu completed=%" PRIu64
               " cpu_user_ms=%.3f cpu_system_ms=%.3f\n", m->enabled, m->seen,
               m->stored, m->completed_total,
               usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0,
               usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0);
}
#endif
