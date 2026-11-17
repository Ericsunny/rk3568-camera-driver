#!/usr/bin/env python3
"""Linux host substitutes for exact MPP input-return matching; no board access."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

SOURCE = (Path(__file__).resolve().parents[1] / 'evrec.c').read_text(encoding='utf-8')


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index('{', start)
    depth = 0
    for i in range(opening, len(SOURCE)):
        depth += (SOURCE[i] == '{') - (SOURCE[i] == '}')
        if not depth:
            return SOURCE[start:i + 1]
    raise ValueError(signature)


PRELUDE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef void *MppTask;
typedef void *MppBuffer;
typedef void *MppPacket;
typedef long long RK_S64;
typedef int MPP_RET;
typedef int MppPollType;
typedef struct { RK_S64 pts; MppBuffer buffer; } Frame;
typedef Frame *MppFrame;
#define CLOCK_MONOTONIC 1
static double now_ms(int clock_id) { (void)clock_id; return 1.0; }
#define MPP_OK 0
#define MPP_NOK -1
#define MPP_PORT_INPUT 0
#define MPP_PORT_OUTPUT 1
#define KEY_INPUT_FRAME 2
#define KEY_OUTPUT_PACKET 3
#define DMA_BUF_IOCTL_SYNC 7
struct dma_buf_sync { unsigned long long flags; };
static int g_stop, sync_calls, sync_fail_count;
static int ioctl(int fd, unsigned long req, void *arg) {
    (void)fd; (void)req; (void)arg; sync_calls++;
    if (sync_calls <= sync_fail_count) { errno=EAGAIN; return -1; }
    return 0;
}
static MppTask g_input_task, g_submitted_task, g_unused_tasks[32];
static unsigned g_unused_count;
static MppFrame g_shared_frame, mock_frame;
static MppPacket g_shared_packet, mock_packet;
static MppBuffer g_task_packet_buffer;
static unsigned long long g_input_returned;
static void *g_ctx;
static int polls_fail, queues_fail, idle_first, dequeues, destroyed, partition, eoi, packet_null;
static int mock_poll(void *ctx, int port, int wait) {
    (void)ctx; (void)port; (void)wait; return polls_fail ? -1 : 0;
}
static int mock_dequeue(void *ctx, int port, MppTask *task) {
    (void)ctx;
    *task = port ? (void *)9 : (idle_first && dequeues++ == 0 ? (void *)8 : g_submitted_task);
    return 0;
}
static int mock_enqueue(void *ctx, int port, MppTask task) {
    (void)ctx; (void)port; (void)task; return queues_fail ? -1 : 0;
}
static struct { int (*poll)(void *,int,int); int (*dequeue)(void *,int,MppTask *);
                int (*enqueue)(void *,int,MppTask); } api = { mock_poll, mock_dequeue, mock_enqueue }, *g_mpi=&api;
static int mpp_task_meta_get_frame(MppTask task, int key, MppFrame *frame) {
    (void)task; (void)key; *frame=mock_frame; return 0;
}
static int mpp_task_meta_get_packet(MppTask task, int key, MppPacket *packet) {
    (void)task; (void)key; *packet=mock_packet; return 0;
}
static int mpp_task_meta_set_frame(MppTask task, int key, MppFrame frame) {
    (void)task; (void)key; (void)frame; return 0;
}
static int mpp_task_meta_set_packet(MppTask task, int key, MppPacket packet) {
    (void)task; (void)key; (void)packet; return 0;
}
static int mpp_packet_init_with_buffer(MppPacket *packet, MppBuffer buffer) {
    (void)buffer; *packet=packet_null ? NULL : (void *)6; return 0;
}
static void mpp_packet_set_length(MppPacket packet, int length) { (void)packet; (void)length; }
static int mpp_packet_is_partition(MppPacket packet) { (void)packet; return partition; }
static int mpp_packet_is_eoi(MppPacket packet) { (void)packet; return eoi; }
static RK_S64 mpp_frame_get_pts(MppFrame frame) { return frame->pts; }
static MppBuffer mpp_frame_get_buffer(MppFrame frame) { return frame->buffer; }
static void mpp_frame_deinit(MppFrame *frame) { *frame=NULL; destroyed++; }
static void check(int ok, const char *why) { if (!ok) { fprintf(stderr,"FAIL %s\n",why); exit(1); } }
'''

TEST = r'''
int main(void) {
    Frame good={1000,(void *)5}, wrong=good;
    MppPacket packet=NULL;
    sync_fail_count=3; sync_calls=0;
    check(shared_cpu_sync(1,0)==0 && sync_calls==4, "transient sync retries");
    sync_fail_count=10; sync_calls=0;
    check(shared_cpu_sync(1,0)<0 && sync_calls==4, "sync retries are bounded");
    g_stop=1; sync_calls=0;
    check(shared_cpu_sync(1,0)<0 && sync_calls==1, "stop does not spin on sync");
    g_stop=0;
    g_input_task=(void *)7; packet_null=1;
    check(shared_submit(&good)!=0 && g_shared_frame==&good && !g_submitted_task,
          "missing packet retains frame for cleanup");
    g_shared_frame=NULL; packet_null=0;
    g_input_task=(void *)7;
    check(shared_submit(&good)==0 && g_submitted_task==(void *)7 && !g_input_task,
          "submitted input stays owned by encoder");
    check(shared_submit(&wrong)!=0 && g_shared_frame==&good,
          "second outstanding submission rejected without losing original ownership");
    mock_packet=g_shared_packet;
    partition=1; eoi=0;
    check(shared_get_packet(&packet)!=0 && !packet, "partial task output rejected");
    partition=0;
    check(shared_get_packet(&packet)==0 && packet==mock_packet && !g_shared_packet,
          "returned output identity and ownership transfer");
    mock_frame=&wrong;
    check(shared_return_input((void *)5,1000)<0 && !g_input_returned && !destroyed,
          "different frame pointer never authorizes reuse");
    mock_frame=&good; good.pts=1001;
    check(shared_return_input((void *)5,1000)<0 && !g_input_returned,
          "wrong PTS never authorizes reuse");
    good.pts=1000; good.buffer=(void *)6;
    check(shared_return_input((void *)5,1000)<0 && !g_input_returned,
          "wrong buffer never authorizes reuse");
    good.buffer=(void *)5; polls_fail=1;
    check(shared_return_input((void *)5,1000)<0 && !g_input_returned,
          "timeout never authorizes reuse");
    polls_fail=0; idle_first=1; dequeues=0;
    check(shared_return_input((void *)5,1000)==0 && g_input_returned==1 && destroyed==1 &&
          !g_shared_frame && !g_submitted_task && g_input_task==(void *)7 && g_unused_count==1,
          "only exact returned task frame PTS buffer authorizes reuse");
    puts("MPP shared input ownership: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='evrec-dmabuf-host-') as tmp:
    src = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    src.write_text(PRELUDE + '\n' + '\n'.join(function(sig) for sig in (
        'static int shared_cpu_sync(', 'static MPP_RET shared_submit(', 'static MPP_RET shared_get_packet(',
        'static int shared_return_input(')) + '\n' + TEST, encoding='utf-8')
    flags = shlex.split(os.environ.get('TEST_CFLAGS', ''))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                    str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
