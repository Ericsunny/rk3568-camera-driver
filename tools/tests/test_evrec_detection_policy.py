"""Exercise the actual result-consumption function with controlled messages."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
text=(root/'tools/evrec_detect_pipeline.h').read_text(encoding='utf-8')
def function(name):
    a=text.index(name);opening=text.index('{',a);depth=0
    for i in range(opening,len(text)):
        depth+=(text[i]=='{')-(text[i]=='}')
        if depth==0:return text[a:i+1]
    raise ValueError(name)
pre=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include "evrec_pipeline.h"
#define DP_MAX_AGE_NS UINT64_C(200000000)
#define EVM_CAPACITY 4096
static struct ep_queue g_dp;
static unsigned g_capture_epoch=1;
static uint64_t g_dp_seen,g_dp_stale,g_dp_epoch_reject,g_dp_applied,g_dp_trigger_applied_ns;
static struct ep_job g_dp_previous,g_dp_trigger_source;
static int g_dp_have_previous,g_streak,g_det_announced,g_trigger,g_det_frames=3;
struct dp_observation {struct ep_result r;uint64_t applied_ns;int accepted;};
static struct dp_observation *g_dp_rows;
static size_t g_dp_stored;
static void npu_apply(const struct ep_detection *d)
{if(d->hit){g_streak++;if(g_streak>=3)g_trigger=1;}else g_streak=0;}
'''
tests=r'''
static void input(unsigned epoch,unsigned seq,unsigned driver,int stale)
{
    struct ep_result r={0};
    r.job.epoch=epoch;r.job.seq=seq;r.job.driver_seq=driver;
    r.job.dq_ns=ep_now()-(stale?DP_MAX_AGE_NS+1000000:0);r.detection.hit=1;
    g_dp.result[(g_dp.head+g_dp.count)%EP_RESULTS]=r;g_dp.count++;
    assert(!dp_drain());
}
int main(void)
{
    pthread_mutex_init(&g_dp.lock,NULL);
    input(1,0,10,0);input(1,1,11,0);input(1,2,12,0);
    assert(g_trigger&&g_streak==3&&g_dp_trigger_source.seq==2);
    input(1,3,13,1);assert(!g_trigger&&!g_streak&&g_dp_stale==1);
    input(1,4,14,0);assert(g_streak==1); /* missing source resets continuity */
    input(0,5,15,0);assert(!g_streak&&g_dp_epoch_reject==1);
    input(1,5,15,0);assert(g_streak==1);
    input(1,5,15,0);assert(!g_streak&&g_dp_stale==2); /* duplicate */
    input(1,6,18,0);assert(g_streak==1); /* driver gap */
    g_capture_epoch=2;input(2,7,0,0);assert(g_streak==1);
    g_dp.stats.failed=1;assert(dp_drain()<0);
    pthread_mutex_destroy(&g_dp.lock);
    puts("PASS: three hits, stale/duplicate/old epoch rejection, gaps reset, worker error propagation");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='detection-policy-') as tmp:
    p=Path(tmp)/'test.c';p.write_text(pre+function('static void dp_reset_streak(')+'\n'+function('static int dp_drain(')+tests)
    binary=Path(tmp)/'test'
    subprocess.run(['gcc','-O1','-g','-Wall','-Werror','-pthread','-fsanitize=address,undefined',
                    '-I',str(root/'tools'),str(p),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
