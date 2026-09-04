#include "nb_live.h"

#include <assert.h>
#include <stdio.h>

int main(void){
    assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_MEDIA,5000000,462000)==589824);
    assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_MEDIA,0,462000)==262144);
    assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_MEDIA,100000000,2000000)==1048576);
    assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_CTRL,5000000,462000)==262144);
    assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_BULK,5000000,462000)==262144);
    assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_MEDIA,UINT64_MAX,16140901064495857663ULL)==1048576);
    nb_live_queue_limits_t ctrl=nb_live_queue_limits(NB_FLOW_CLASS_CTRL,0);
    nb_live_queue_limits_t media=nb_live_queue_limits(NB_FLOW_CLASS_MEDIA,1);
    nb_live_queue_limits_t bulk=nb_live_queue_limits(NB_FLOW_CLASS_BULK,0);
    assert(ctrl.high_bytes==64*1024&&ctrl.low_bytes==32*1024);
    assert(media.high_bytes==256*1024&&media.low_bytes==128*1024&&media.deadline_us==200000);
    assert(nb_live_queue_limits(NB_FLOW_CLASS_MEDIA,0).deadline_us==500000);
    assert(bulk.high_bytes==1024*1024&&bulk.low_bytes==512*1024);
    assert(nb_live_queue_limits_for_path(NB_FLOW_CLASS_MEDIA,1,100000).deadline_us==200000);
    assert(nb_live_queue_limits_for_path(NB_FLOW_CLASS_MEDIA,1,573000).deadline_us==200000);
    assert(nb_live_queue_limits_for_path(NB_FLOW_CLASS_MEDIA,1,900000).deadline_us==200000);
    assert(nb_live_queue_limits_for_path(NB_FLOW_CLASS_CTRL,1,573000).deadline_us==100000);
    assert(nb_live_queue_limits_for_path(NB_FLOW_CLASS_MEDIA,0,573000).deadline_us==500000);
    assert(!nb_live_queue_expiry_enabled(NB_FLOW_CLASS_MEDIA,1));
    assert(nb_live_queue_expiry_enabled(NB_FLOW_CLASS_MEDIA,0));
    assert(nb_live_queue_expiry_enabled(NB_FLOW_CLASS_CTRL,1));
    assert(nb_live_queue_expiry_enabled(NB_FLOW_CLASS_BULK,1));
    nb_live_queue_clock_t clock={0};
    nb_live_queue_appended(&clock,0,100,1000);assert(nb_live_queue_age_us(&clock,100,6000)==5000);
    nb_live_queue_appended(&clock,100,50,10000);assert(nb_live_queue_age_us(&clock,150,20000)==19000);
    nb_live_queue_consumed(&clock,100,50);assert(nb_live_queue_age_us(&clock,50,20000)==10000);
    nb_live_queue_consumed(&clock,50,0);assert(clock.count==0);

    nb_live_flow_runtime_t runtime={0};double c2s=0,s2c=0;
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_CTRL,0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_CTRL,0,40000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_CTRL,0,80000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_DEMOTE_BULK);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_BULK,0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_BULK,40000,0,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_BULK,80000,0,3000000,&c2s,&s2c)==NB_LIVE_FLOW_PROMOTE_MEDIA);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_MEDIA,0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_MEDIA,8000,150000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_MEDIA,16000,300000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_DEMOTE_DOWNLINK);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_CTRL,"api*",0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_CTRL,"api*",0,40000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_CTRL,"api*",0,80000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_CTRL,"log*",0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_CTRL,"log*",0,40000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_CTRL,"log*",0,80000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_DEMOTE_BULK);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_MEDIA,"frontier*",0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_MEDIA,"frontier*",8000,150000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_MEDIA,"frontier*",16000,300000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_MEDIA,"teko*",0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_MEDIA,"teko*",8000,150000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe_rule(&runtime,NB_FLOW_CLASS_MEDIA,"teko*",16000,300000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_DEMOTE_DOWNLINK);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_MEDIA,0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_MEDIA,128000,16000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_MEDIA,256000,32000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);

    runtime=(nb_live_flow_runtime_t){0};
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_BULK,0,0,1000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_BULK,40000,400000,2000000,&c2s,&s2c)==NB_LIVE_FLOW_KEEP);
    assert(nb_live_flow_observe(&runtime,NB_FLOW_CLASS_BULK,80000,800000,3000000,&c2s,&s2c)==NB_LIVE_FLOW_PROMOTE_MEDIA);

    nb_live_sched_t sched={0};
    assert(nb_live_sched_grant(&sched,NB_FLOW_CLASS_CTRL,64*1024,1000000,1)==32*1024);
    assert(nb_live_sched_grant(&sched,NB_FLOW_CLASS_CTRL,4096,1000000,1)==0);
    assert(nb_live_sched_grant(&sched,NB_FLOW_CLASS_MEDIA,4096,1000000,1)==4096);
    assert(nb_live_sched_grant(&sched,NB_FLOW_CLASS_CTRL,4096,1100000,0)==4096);
    assert(nb_live_sched_ctrl_ready(&sched,1110000));
    puts("nb_live tests passed");return 0;
}
