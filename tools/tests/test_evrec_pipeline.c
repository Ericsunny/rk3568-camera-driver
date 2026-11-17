#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "../evrec_pipeline.h"
struct fake { pthread_mutex_t lock; pthread_cond_t wake; int entered, release, bad; unsigned data[2]; };
static int blocked(int slot, const struct ep_job *job, struct ep_detection *d, void *arg)
{
    struct fake *f=arg;
    pthread_mutex_lock(&f->lock);f->entered=1;pthread_cond_broadcast(&f->wake);
    while(!f->release)pthread_cond_wait(&f->wake,&f->lock);
    if(f->data[slot]!=job->seq)f->bad=1;
    pthread_mutex_unlock(&f->lock);d->nd=(int)job->seq;
    return 0;
}
static int fail(int slot,const struct ep_job *j,struct ep_detection *d,void *arg)
{(void)slot;(void)j;(void)d;(void)arg;return -1;}
static int quick(int slot,const struct ep_job *j,struct ep_detection *d,void *arg)
{(void)slot;(void)j;(void)d;(void)arg;return 0;}
static void until_done(struct ep_queue *q,uint64_t n)
{
    uint64_t end=ep_now()+UINT64_C(2000000000);
    while(ep_now()<end){struct ep_stats s=ep_snapshot(q);if(s.completed>=n||s.failed)return;usleep(1000);}
    assert(!"worker deadline");
}
int main(void)
{
    struct ep_queue q;struct ep_result r;struct ep_job job={0};
    struct fake f={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0,0,0,{0,0}};
    int first,second,third;
    assert(!ep_open(&q,blocked,&f));first=ep_reserve(&q);assert(first>=0);
    f.data[first]=0;assert(!ep_submit(&q,first,&job));
    pthread_mutex_lock(&f.lock);while(!f.entered)pthread_cond_wait(&f.wake,&f.lock);pthread_mutex_unlock(&f.lock);
    second=ep_reserve(&q);assert(second>=0&&second!=first);job.seq=1;f.data[second]=1;assert(!ep_submit(&q,second,&job));
    third=ep_reserve(&q);assert(third==second);job.seq=2;f.data[third]=2;assert(!ep_submit(&q,third,&job));
    assert(ep_join(&q,20)<0&&errno==ETIMEDOUT); /* ownership survives timeout */
    pthread_mutex_lock(&f.lock);f.release=1;pthread_cond_broadcast(&f.wake);pthread_mutex_unlock(&f.lock);
    assert(!ep_join(&q,2000));assert(!f.bad);
    assert(ep_pop(&q,&r)==1&&r.job.seq==0&&r.detection.nd==0);
    assert(ep_pop(&q,&r)==1&&r.job.seq==2&&r.detection.nd==2);
    assert(ep_snapshot(&q).replaced==1);assert(ep_pop(&q,&r)==0);ep_destroy(&q);
    assert(!ep_open(&q,fail,NULL));first=ep_reserve(&q);assert(!ep_submit(&q,first,&job));until_done(&q,1);
    assert(ep_snapshot(&q).failed);assert(ep_reserve(&q)<0);assert(!ep_join(&q,2000));ep_destroy(&q);
    assert(!ep_open(&q,quick,NULL));
    for(unsigned i=0;i<EP_RESULTS+1;i++){first=ep_reserve(&q);assert(first>=0);job.seq=i;assert(!ep_submit(&q,first,&job));until_done(&q,i+1);}
    assert(ep_snapshot(&q).failed);assert(!ep_join(&q,2000));ep_destroy(&q);
    pthread_cond_destroy(&f.wake);pthread_mutex_destroy(&f.lock);
    puts("PASS: running input immutable; pending replacement; timeout ownership; callback error; result overflow");
    return 0;
}
