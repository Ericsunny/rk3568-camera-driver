#define _GNU_SOURCE
#include "evrec_metrics.h"
#include <assert.h>

int main(int argc, char **argv)
{
    struct evm_context m = {0}, disabled = {0}, collision = {0}, full = {0};
    struct evm_row *r;
    char path[1024];
    unsigned i;
    assert(argc == 2);
    assert(!evm_begin(&disabled, 0, 0, 0, 0, 0, 0));
    assert(!disabled.rows && !disabled.seen && !evm_finish(&disabled));
    snprintf(path, sizeof(path), "%s/frames.csv", argv[1]);
    assert(!evm_open(&m, path));
    assert(evm_open(&collision, path) < 0 && errno == EEXIST);
    evm_new_epoch(&m);
    r = evm_begin(&m, 0, UINT32_MAX, 2, 0x2000, 12345, 1);
    assert(r && r->driver_seq == UINT32_MAX && r->index == 2 && r->driver_ns == 12345);
    evm_encoded(r); evm_input_returned(r); evm_qbuf(r); evm_completed(&m, r);
    assert(r->dq_ns <= r->encode_ns && r->encode_ns <= r->returned_ns && r->returned_ns <= r->qbuf_ns);
    evm_new_epoch(&m);
    for (i = 1; i < EVM_CAPACITY + 1; ++i) {
        r = evm_begin(&m, i, i, 0, 0, 0, 0);
        evm_qbuf(r); evm_encoded(r);
        if (i < EVM_CAPACITY - 1) evm_completed(&m, r);
    }
    assert(m.seen == EVM_CAPACITY + 1 && m.stored == EVM_CAPACITY && m.epoch == 2);
    assert(!evm_finish(&m) && !evm_finish(&m));
    snprintf(path, sizeof(path), "%s/full.csv", argv[1]);
    assert(!evm_open(&full, path));
    fclose(full.csv);
    full.csv = fopen("/dev/full", "w");
    assert(full.csv);
    r = evm_begin(&full, 0, 0, 0, 0, 0, 0);
    evm_qbuf(r); evm_encoded(r); evm_completed(&full, r);
    assert(evm_finish(&full) < 0);
    evm_report(&disabled);
    puts("PASS bounded observations, exclusive output, time ordering and write failure");
    return 0;
}
