/* Private same-executable SOCK_SEQPACKET channel. Only the recorder changes event state. */
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
struct side_packet {
    uint32_t generation, driver_seq;
    uint64_t driver_ns;
    struct ep_detection detection;
    uint64_t ready_ns;
};
static struct {
    int fd;
    pid_t pid;
    unsigned generation, last_seq;
    uint64_t received, applied, retry_at, retired_at;
    int previous, online;
    unsigned latest_slot;
    int pending;
    struct side_packet pending_packet;
    FILE *log;
    struct { struct ep_job job; uint64_t stamp; } history[16];
} side = { .fd = -1 };

static int side_worker(char **av)
{
    unsigned generation = (unsigned)strtoul(av[2], NULL, 10);
    pid_t parent = (pid_t)strtol(av[7], NULL, 10);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) return 1;
    /* exec started a fresh SDK context. Drop inherited recorder/device fds. */
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;
    if (!dir) return 1;
    while ((entry = readdir(dir))) {
        int fd = atoi(entry->d_name);
        if (fd > 3 && fd != dirfd(dir)) close(fd);
    }
    closedir(dir);
    g_model = av[4]; g_det_score = strtof(av[5], NULL); g_obj_thresh = strtof(av[6], NULL);
    g_side_contiguous = !strcmp(av[8],"contiguous");
    fprintf(stderr,"selfpath worker scan=%s\n",av[8]);
    g_input_dmabuf = g_npu_input_dmabuf = g_detect_pipeline = 1;
    if (npu_init()) return 1;
    memset(g_rgb, 114, g_npu_rgb_mem->size);
    if (rknn_mem_sync(g_nctx,g_npu_rgb_mem,RKNN_MEMORY_SYNC_TO_DEVICE) ||
        cap_open(av[3],640,360,4) || cap.w != 640 || cap.h != 360) return 1;
    for (;;) {
        struct pollfd p = { .fd = cap.fd, .events = POLLIN };
        struct v4l2_plane plane = {0};
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_DMABUF, .length = 1, .m.planes = &plane };
        if (poll(&p,1,1000) != 1 || !(p.revents & POLLIN) || xioctl(cap.fd,VIDIOC_DQBUF,&b) ||
            b.index >= (unsigned)cap.nbufs || b.flags & V4L2_BUF_FLAG_ERROR ||
            (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) break;
        struct side_packet msg = { .generation = generation, .driver_seq = b.sequence,
            .driver_ns = (uint64_t)b.timestamp.tv_sec*1000000000 + b.timestamp.tv_usec*1000ULL };
        int fd = mpp_buffer_get_fd(cap.input[b.index]);
        if (fd < 0 || rga_scale_to(cap.map[b.index],cap.w,cap.h,cap.stride,fd,g_npu_rgb_mem)) break;
        plane.m.fd = fd; plane.length = cap.len[b.index];
        if (xioctl(cap.fd,VIDIOC_QBUF,&b) || npu_infer(g_rgb,g_npu_rgb_mem,1,&msg.detection)) break;
        msg.ready_ns = ep_now();
        if (send(3,&msg,sizeof(msg),MSG_NOSIGNAL) != sizeof(msg) && errno != EAGAIN) break;
    }
    cap_release();
    return 1; /* Supervisor reopens only this branch. */
}

static int side_start(void)
{
    int pair[2];
    char gen[16], parent[24], score[32], obj[32];
    posix_spawn_file_actions_t act;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair)) return -1;
    snprintf(gen,sizeof(gen),"%u",++side.generation);
    snprintf(parent,sizeof(parent),"%ld",(long)getpid());
    snprintf(score,sizeof(score),"%.9g",g_det_score);
    snprintf(obj,sizeof(obj),"%.9g",g_obj_thresh);
    char *args[] = {"evrec","--selfpath-worker",gen,(char*)g_side_dev,(char*)g_model,score,obj,parent,
                    g_side_contiguous ? "contiguous" : "reference",NULL};
    int rc = posix_spawn_file_actions_init(&act);
    if (!rc) {
        rc = posix_spawn_file_actions_adddup2(&act,pair[1],3);
        if (!rc) rc = posix_spawn(&side.pid,"/proc/self/exe",&act,NULL,args,environ);
        posix_spawn_file_actions_destroy(&act);
    }
    close(pair[1]);
    if (rc) { close(pair[0]); side.pid = 0; return -1; }
    side.fd = pair[0]; side.received = ep_now(); side.retired_at = 0;
    fprintf(stderr,"selfpath start gen=%u pid=%ld ns=%llu\n",side.generation,(long)side.pid,(unsigned long long)side.received);
    return 0;
}

static void side_reset(void)
{
    side.previous = side.online = 0;
    dp_reset_streak();
}
static void side_retire(void)
{
    side.pending = 0;
    if (side.fd >= 0) { close(side.fd); side.fd = -1; }
    side_reset();
    if (side.pid > 0 && !side.retired_at) {
        side.retired_at = ep_now(); kill(side.pid,SIGTERM);
        fprintf(stderr,"selfpath retire gen=%u ns=%llu\n",side.generation,(unsigned long long)side.retired_at);
    }
}
static void side_tick(void)
{
    uint64_t now = ep_now();
    if (side.pid > 0) {
        int status;
        pid_t r = waitpid(side.pid,&status,WNOHANG);
        if (r == side.pid) {
            fprintf(stderr,"selfpath exit gen=%u status=%d ns=%llu\n",side.generation,status,(unsigned long long)now);
            side.pid = 0; side_retire(); side.retry_at = now + 500000000;
        }
    }
    if (!side.pid) {
        if (now >= side.retry_at && side_start()) {
            perror("selfpath spawn"); side.retry_at = now + 1000000000;
        }
        return;
    }
    if (side.retired_at) {
        if (now - side.retired_at > 500000000) kill(side.pid,SIGKILL);
        return;
    }
    if (now - side.received > 2000000000) { side_retire(); return; }
    struct pollfd p = { .fd = side.fd, .events = POLLIN };
    if (poll(&p,1,0) < 0 || (p.revents & (POLLHUP|POLLERR|POLLNVAL))) { side_retire(); return; }
    for (int i = 0; i < 16; ++i) {
        struct side_packet m;
        if (side.pending) {
            m = side.pending_packet;
            side.pending = 0;
        } else {
            ssize_t n = recv(side.fd,&m,sizeof(m),MSG_DONTWAIT|MSG_TRUNC);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            if (n != sizeof(m)) { side_retire(); break; }
            side.received = ep_now();
        }
        now = ep_now();
        unsigned slot = m.driver_seq % 16;
        struct ep_job j = side.history[slot].job;
        const char *result = "accepted";
        if (m.generation != side.generation) result = "generation";
        else if (!m.driver_ns || now < m.driver_ns || now-m.driver_ns > DP_MAX_AGE_NS) result = "age";
        else if (g_side_poll && side.history[side.latest_slot].job.epoch == g_capture_epoch &&
                 (int32_t)(m.driver_seq-side.history[side.latest_slot].job.driver_seq) > 0 &&
                 m.driver_ns > side.history[side.latest_slot].stamp) {
            /* The ISP result beat the matching main DQBUF. Retry after capture;
             * leave further packets on the socket and omit it from the wait set. */
            side.pending_packet = m;
            side.pending = 1;
            break;
        }
        else if (j.epoch != g_capture_epoch || j.driver_seq != m.driver_seq ||
                 m.driver_ns != side.history[slot].stamp) result = "unmatched";
        else if (side.previous && (int32_t)(m.driver_seq-side.last_seq) <= 0) result = "order";
        if (!strcmp(result,"accepted")) {
            if (!side.previous || m.driver_seq != side.last_seq+1) dp_reset_streak();
            npu_apply(&m.detection);
            side.previous = side.online = 1; side.last_seq = m.driver_seq; side.applied = now;
            if (m.detection.hit && g_trigger && g_streak >= g_det_frames) {
                g_dp_trigger_source = j; g_dp_trigger_applied_ns = now;
            }
        }
        if (side.log) fprintf(side.log,"%u,%u,%u,%llu,%u,%u,%llu,%d,%.6f,%s,%llu\n",
            side.generation,m.generation,m.driver_seq,(unsigned long long)m.driver_ns,j.epoch,j.seq,
            (unsigned long long)now,m.detection.hit,(double)m.detection.score,result,(unsigned long long)m.ready_ns);
    }
    if (side.online && ep_now()-side.applied > DP_MAX_AGE_NS) side_reset();
}
static void side_stop(void)
{
    side_retire();
    for (int i=0; side.pid > 0 && i<100; ++i) {
        if (waitpid(side.pid,NULL,WNOHANG)==side.pid) { side.pid=0; break; }
        if (i==25) kill(side.pid,SIGKILL);
        usleep(10000);
    }
    if (side.log) { fclose(side.log); side.log=NULL; }
}
