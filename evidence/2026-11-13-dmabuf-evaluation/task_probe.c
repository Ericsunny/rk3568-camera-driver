/* Synthetic, no camera: investigate old board MPP task completion semantics. */
#define main evrec_original_main
#include "evrec.c"
#undef main
#include <rockchip/mpp_task.h>
#include <rockchip/mpp_meta.h>

int main(void)
{
    MppTask task = NULL;
    MppFrame frame = NULL, returned_frame = NULL;
    MppPacket packet = NULL, returned_packet = NULL;
    MppBuffer packet_buffer = NULL;
    MPP_RET r;
    int rc = 1, enqueued = 0;
    if (enc_init(640, 480, 640, 1000, 15, 15) != 0) goto done;
    if (!g_mpi->poll || !g_mpi->dequeue || !g_mpi->enqueue) {
        fprintf(stderr, "task API absent\n"); goto done;
    }
    memset(mpp_buffer_get_ptr(g_fbuf), 128, g_fsize);
    if (mpp_buffer_get(g_grp, &packet_buffer, g_fsize) != MPP_OK ||
        mpp_packet_init_with_buffer(&packet, packet_buffer) != MPP_OK ||
        mpp_frame_init(&frame) != MPP_OK) goto done;
    mpp_packet_set_length(packet, 0);
    mpp_frame_set_width(frame, 640); mpp_frame_set_height(frame, 480);
    mpp_frame_set_hor_stride(frame, 640); mpp_frame_set_ver_stride(frame, 480);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, g_fbuf); mpp_frame_set_pts(frame, 123456);
    r = g_mpi->poll(g_ctx, MPP_PORT_INPUT, (MppPollType)2000);
    printf("input_poll=%d\n", r); fflush(stdout); if (r != MPP_OK) goto done;
    r = g_mpi->dequeue(g_ctx, MPP_PORT_INPUT, &task);
    printf("input_dequeue=%d task=%p\n", r, (void*)task); fflush(stdout);
    if (r != MPP_OK || !task) goto done;
    if (mpp_task_meta_set_frame(task, KEY_INPUT_FRAME, frame) != MPP_OK ||
        mpp_task_meta_set_packet(task, KEY_OUTPUT_PACKET, packet) != MPP_OK) goto done;
    r = g_mpi->enqueue(g_ctx, MPP_PORT_INPUT, task); task = NULL;
    printf("input_enqueue=%d\n", r); fflush(stdout); if (r != MPP_OK) goto done;
    enqueued = 1;
    r = g_mpi->poll(g_ctx, MPP_PORT_OUTPUT, (MppPollType)2000);
    printf("output_poll=%d\n", r); fflush(stdout); if (r != MPP_OK) goto done;
    r = g_mpi->dequeue(g_ctx, MPP_PORT_OUTPUT, &task);
    printf("output_dequeue=%d task=%p\n", r, (void*)task); fflush(stdout);
    if (r != MPP_OK || !task) goto done;
    {
        MPP_RET rf = mpp_task_meta_get_frame(task, KEY_INPUT_FRAME, &returned_frame);
        MPP_RET rp = mpp_task_meta_get_packet(task, KEY_OUTPUT_PACKET, &returned_packet);
        printf("returned_frame_rc=%d ptr=%p same=%d returned_packet_rc=%d ptr=%p same=%d len=%zu\n",
               rf, (void*)returned_frame, returned_frame == frame, rp, (void*)returned_packet,
               returned_packet == packet, returned_packet ? mpp_packet_get_length(returned_packet) : 0);
        fflush(stdout);
        if (rf == MPP_OK && returned_frame == frame && rp == MPP_OK && returned_packet &&
            mpp_packet_get_length(returned_packet) > 0) rc = 0;
    }
    r = g_mpi->enqueue(g_ctx, MPP_PORT_OUTPUT, task); task = NULL;
    printf("output_enqueue=%d\n", r); fflush(stdout);
    if (r != MPP_OK) rc = 1;
done:
    /* Complete context before disposing app-owned input/output resources. */
    if (g_ctx) { mpp_destroy(g_ctx); g_ctx = NULL; }
    if (frame) mpp_frame_deinit(&frame);
    if (packet) mpp_packet_deinit(&packet);
    if (packet_buffer) mpp_buffer_put(packet_buffer);
    enc_release();
    printf("TASK_PROBE_RC=%d enqueued=%d\n",rc,enqueued); fflush(stdout);
    return rc;
}
