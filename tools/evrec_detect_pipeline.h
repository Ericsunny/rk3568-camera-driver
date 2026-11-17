/* Included after RGA and inference helpers; only the worker owns RKNN calls. */
#ifndef EVREC_DETECT_PIPELINE_H
#define EVREC_DETECT_PIPELINE_H
#define DP_MAX_AGE_NS UINT64_C(200000000)
static struct ep_queue g_dp;
static rknn_tensor_mem *g_dp_mem[EP_SLOTS];
static int g_dp_started;
static uint64_t g_dp_eligible, g_dp_applied, g_dp_stale, g_dp_epoch_reject;
static struct ep_job g_dp_previous;
static int g_dp_have_previous;
struct dp_observation { struct ep_result r; uint64_t applied_ns; int accepted; };
static struct dp_observation *g_dp_rows;
static size_t g_dp_stored;
static uint64_t g_dp_seen;
static FILE *g_dp_csv, *g_dp_meta;

static int dp_infer(int slot, const struct ep_job *job, struct ep_detection *d, void *unused)
{
    (void)job; (void)unused;
    return npu_infer(g_dp_mem[slot]->virt_addr, g_dp_mem[slot], 1, d);
}
static int dp_start(void)
{
    size_t size = (size_t)g_npu_w * g_npu_h * 3;
    int i;
    char *path = NULL;
    g_dp_mem[0] = g_npu_rgb_mem;
    g_dp_mem[1] = rknn_create_mem(g_nctx, (uint32_t)size);
    for (i = 0; i < EP_SLOTS; i++) {
        if (!g_dp_mem[i] || !g_dp_mem[i]->virt_addr || g_dp_mem[i]->fd < 0 ||
            g_dp_mem[i]->size < size ||
            rknn_mem_sync(g_nctx, g_dp_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE) != RKNN_SUCC)
            return -1;
        memset(g_dp_mem[i]->virt_addr, 114, size);
        if (rknn_mem_sync(g_nctx, g_dp_mem[i], RKNN_MEMORY_SYNC_TO_DEVICE) != RKNN_SUCC)
            return -1;
    }
    if (g_frame_metrics_path) {
        path = malloc(strlen(g_frame_metrics_path) + 32);
        g_dp_rows = calloc(EVM_CAPACITY, sizeof(*g_dp_rows));
        if (!path || !g_dp_rows) { free(path); return -1; }
        sprintf(path, "%s.detect.csv", g_frame_metrics_path);
        g_dp_csv = evm_exclusive(path);
        sprintf(path, "%s.detect.meta", g_frame_metrics_path);
        g_dp_meta = evm_exclusive(path); free(path);
        if (!g_dp_csv || !g_dp_meta) return -1;
    }
    if (ep_open(&g_dp, dp_infer, NULL)) return -1;
    g_dp_started = 1;
    return 0;
}
static void dp_reset_streak(void)
{
    g_streak = 0; g_det_announced = 0; g_trigger = 0;
}
static int dp_drain(void)
{
    struct ep_result r;
    int rc;
    while ((rc = ep_pop(&g_dp, &r)) == 1) {
        uint64_t now = ep_now();
        int accepted = 1;
        if (r.job.epoch != g_capture_epoch) {
            accepted = 0; g_dp_epoch_reject++;
        } else if (now < r.job.dq_ns || now - r.job.dq_ns > DP_MAX_AGE_NS ||
                   (g_dp_have_previous && r.job.epoch == g_dp_previous.epoch &&
                    (int32_t)(r.job.seq - g_dp_previous.seq) <= 0)) {
            accepted = 0; g_dp_stale++;
        }
        if (!accepted) dp_reset_streak();
        else {
            if (g_dp_have_previous &&
                (r.job.epoch != g_dp_previous.epoch ||
                 r.job.seq != g_dp_previous.seq + 1 ||
                 r.job.driver_seq != g_dp_previous.driver_seq + 1))
                dp_reset_streak();
            npu_apply(&r.detection);
            if (r.detection.hit && g_streak >= g_det_frames && g_trigger) {
                g_dp_trigger_source = r.job; g_dp_trigger_applied_ns = now;
            }
            g_dp_previous = r.job; g_dp_have_previous = 1; g_dp_applied++;
        }
        g_dp_seen++;
        if (g_dp_rows && g_dp_stored < EVM_CAPACITY) {
            struct dp_observation *o = &g_dp_rows[g_dp_stored++];
            o->r = r; o->applied_ns = now; o->accepted = accepted;
        }
    }
    return rc < 0 ? -1 : 0;
}
static int dp_stop(void)
{
    struct ep_stats s;
    int failed;
    size_t i;
    if (!g_dp_started) return 0;
    /* A timed-out worker still owns its context and input. Never free them. */
    if (ep_join(&g_dp, 2000)) {
        fprintf(stderr, "检测线程未在2秒内退出，保留未完成产物并失败退出\n");
        fflush(NULL); _exit(1);
    }
    failed = dp_drain() < 0;
    s = ep_snapshot(&g_dp); failed |= s.failed;
    printf("pipeline: eligible=%llu submitted=%llu replaced=%llu completed=%llu applied=%llu stale=%llu epoch_reject=%llu failed=%d\n",
           (unsigned long long)g_dp_eligible, (unsigned long long)s.submitted,
           (unsigned long long)s.replaced, (unsigned long long)s.completed,
           (unsigned long long)g_dp_applied, (unsigned long long)g_dp_stale,
           (unsigned long long)g_dp_epoch_reject, failed);
    if (g_dp_csv) {
        if (fprintf(g_dp_csv, "epoch,frame_seq,driver_sequence,source_dq_ns,media_ms,infer_start_ns,infer_done_ns,applied_ns,accepted,hit,score,input_mean\n") < 0)
            failed = 1;
        for (i = 0; i < g_dp_stored; i++) {
            const struct dp_observation *o = &g_dp_rows[i];
            if (fprintf(g_dp_csv, "%u,%u,%u,%llu,%.3f,%llu,%llu,%llu,%d,%d,%.6f,%.3f\n",
                    o->r.job.epoch, o->r.job.seq, o->r.job.driver_seq,
                    (unsigned long long)o->r.job.dq_ns, o->r.job.media_ms,
                    (unsigned long long)o->r.started_ns, (unsigned long long)o->r.completed_ns,
                    (unsigned long long)o->applied_ns, o->accepted,
                    o->r.detection.hit, (double)o->r.detection.score, o->r.detection.mean) < 0)
                failed = 1;
        }
        if (fclose(g_dp_csv)) failed = 1;
        g_dp_csv = NULL;
    }
    if (g_dp_meta) {
        if (fprintf(g_dp_meta, "{\"schema\":1,\"eligible\":%llu,\"submitted\":%llu,\"replaced\":%llu,\"completed\":%llu,\"applied\":%llu,\"stale\":%llu,\"epoch_reject\":%llu,\"stored\":%zu,\"truncated\":%s,\"failed\":%s,\"max_result_age_ns\":%llu,\"preroll_ring_bytes\":%zu,\"preroll_ring_limit_bytes\":%zu}\n",
                (unsigned long long)g_dp_eligible, (unsigned long long)s.submitted,
                (unsigned long long)s.replaced, (unsigned long long)s.completed,
                (unsigned long long)g_dp_applied, (unsigned long long)g_dp_stale,
                (unsigned long long)g_dp_epoch_reject, g_dp_stored,
                g_dp_seen > g_dp_stored ? "true" : "false", failed ? "true" : "false",
                (unsigned long long)DP_MAX_AGE_NS, ring_bytes, ring_limit) < 0) failed = 1;
        if (fclose(g_dp_meta)) failed = 1;
        g_dp_meta = NULL;
    }
    ep_destroy(&g_dp); g_dp_started = 0;
    return failed ? -1 : 0;
}
static void dp_release(void)
{
    if (dp_stop()) fprintf(stderr, "pipeline cleanup reported failure\n");
    if (g_dp_mem[1]) { rknn_destroy_mem(g_nctx, g_dp_mem[1]); g_dp_mem[1] = NULL; }
    if (g_dp_csv) { fclose(g_dp_csv); g_dp_csv = NULL; }
    if (g_dp_meta) { fclose(g_dp_meta); g_dp_meta = NULL; }
    free(g_dp_rows); g_dp_rows = NULL;
}
static void dp_probe_fill(MppBuffer buffer, unsigned pattern)
{
    unsigned x, y;
    unsigned char *p = mpp_buffer_get_ptr(buffer);
    for (y = 0; y < 1080; y++)
        for (x = 0; x < 1920; x++)
            p[y * 1920 + x] = (unsigned char)(16 + (x / 64 + y / 32 + pattern * 17) % 220);
    memset(p + 1920 * 1080, 90 + pattern * 5, 1920 * 1080 / 2);
}
/* Generated images only: no camera STREAMON or person image is involved. */
static int dp_selfcheck(void)
{
    struct ep_detection expected[12];
    MppBufferGroup group = NULL;
    MppBuffer source = NULL;
    unsigned i;
    int rc = -1, fd;
    g_detect_pipeline = 0;
    if (mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION) != MPP_OK ||
        mpp_buffer_get(group, &source, 1920 * 1080 * 3 / 2) != MPP_OK || !source)
        goto done;
    fd = mpp_buffer_get_fd(source);
    if (fd < 0) goto done;
    for (i = 0; i < 12; i++) {
        if (shared_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) goto done;
        dp_probe_fill(source, i);
        if (shared_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) ||
            rga_scale_to(mpp_buffer_get_ptr(source), 1920, 1080, 1920, fd, g_npu_rgb_mem) ||
            npu_infer(g_rgb, g_npu_rgb_mem, 1, &expected[i])) goto done;
    }
    g_detect_pipeline = 1;
    if (dp_start()) goto done;
    for (i = 0; i < 12; i++) {
        struct ep_job job = {0};
        struct ep_result result;
        uint64_t until;
        int slot = ep_reserve(&g_dp), got = 0;
        if (slot < 0 || shared_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) goto done;
        dp_probe_fill(source, i);
        if (shared_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) ||
            rga_scale_to(mpp_buffer_get_ptr(source), 1920, 1080, 1920, fd, g_dp_mem[slot])) goto done;
        job.seq = i; job.dq_ns = ep_now();
        if (ep_submit(&g_dp, slot, &job)) goto done;
        until = ep_now() + UINT64_C(5000000000);
        while (ep_now() < until && !(got = ep_pop(&g_dp, &result))) usleep(1000);
        if (got != 1 || result.job.seq != i) goto done;
        printf("pipeline-check pattern=%u slot=%d input=%016llx output=%016llx match=%d\n", i, slot,
               (unsigned long long)result.detection.input_hash,
               (unsigned long long)result.detection.output_hash,
               result.detection.input_hash == expected[i].input_hash && result.detection.output_hash == expected[i].output_hash);
        if (result.detection.input_hash != expected[i].input_hash ||
            result.detection.output_hash != expected[i].output_hash) goto done;
    }
    {
        unsigned matched = 0;
        for (i = 0; i < 48; i++) {
            struct ep_job job = {0};
            struct ep_result result;
            int slot = ep_reserve(&g_dp), got;
            if (slot < 0 || shared_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) goto done;
            dp_probe_fill(source, i % 12);
            if (shared_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) ||
                rga_scale_to(mpp_buffer_get_ptr(source), 1920, 1080, 1920, fd, g_dp_mem[slot])) goto done;
            job.seq = 100 + i; job.dq_ns = ep_now();
            if (ep_submit(&g_dp, slot, &job)) goto done;
            usleep(20000);
            while ((got = ep_pop(&g_dp, &result)) == 1) {
                unsigned pattern = (result.job.seq - 100) % 12;
                int match = result.job.seq >= 100 && result.job.seq < 148 &&
                    result.detection.input_hash == expected[pattern].input_hash &&
                    result.detection.output_hash == expected[pattern].output_hash;
                printf("pipeline-burst seq=%u pattern=%u match=%d\n", result.job.seq, pattern, match);
                if (!match) goto done;
                matched++;
            }
            if (got < 0) goto done;
        }
        if (ep_join(&g_dp, 2000)) goto done;
        for (;;) {
            struct ep_result result;
            int got = ep_pop(&g_dp, &result);
            unsigned pattern;
            if (!got) break;
            if (got < 0) goto done;
            pattern = (result.job.seq - 100) % 12;
            if (result.job.seq < 100 || result.job.seq >= 148 ||
                result.detection.input_hash != expected[pattern].input_hash ||
                result.detection.output_hash != expected[pattern].output_hash) goto done;
            printf("pipeline-burst seq=%u pattern=%u match=1\n", result.job.seq, pattern);
            matched++;
        }
        if (matched < 8) goto done;
        printf("pipeline-burst completed=%u offered=48\n", matched);
    }
    rc = 0;
done:
    if (dp_stop()) rc = -1;
    if (source) mpp_buffer_put(source);
    if (group) mpp_buffer_group_put(group);
    printf("pipeline-selfcheck pass=%d cases=12\n", rc == 0);
    return rc;
}
#endif
