#include "nb_live.h"

#define KIB (1024u)
#define MIB (1024u * 1024u)
#define FLOW_SAMPLE_US 1000000ULL
#define CTRL_RATE_BYTES_PER_SEC 125000.0
#define CTRL_BURST_BYTES (32.0 * KIB)
#define QUEUE_SEGMENT_COALESCE_US 5000ULL

nb_live_queue_limits_t nb_live_queue_limits(nb_flow_class_t flow_class, int udp_mode){
    nb_live_queue_limits_t limits;
    if(flow_class==NB_FLOW_CLASS_CTRL){
        limits.high_bytes=64*KIB;limits.low_bytes=32*KIB;limits.deadline_us=100000ULL;
    }else if(flow_class==NB_FLOW_CLASS_MEDIA){
        limits.high_bytes=256*KIB;limits.low_bytes=128*KIB;limits.deadline_us=udp_mode?200000ULL:500000ULL;
    }else{
        limits.high_bytes=MIB;limits.low_bytes=512*KIB;limits.deadline_us=1000000ULL;
    }
    return limits;
}

void nb_live_queue_appended(nb_live_queue_clock_t* clock,size_t previous_len,size_t added,uint64_t now_us){
    if(clock==NULL||added==0)return;
    if(previous_len==0){clock->head=0;clock->count=0;}
    if(clock->count>0){
        size_t tail=(clock->head+clock->count-1)%NB_LIVE_QUEUE_SEGMENTS;
        nb_live_queue_segment_t* segment=&clock->segments[tail];
        if(now_us>=segment->queued_at&&now_us-segment->queued_at<=QUEUE_SEGMENT_COALESCE_US){
            segment->bytes+=added;return;
        }
    }
    if(clock->count==NB_LIVE_QUEUE_SEGMENTS){
        /* The 5 ms buckets cover the 300-500 ms target window. If a caller
         * exceeds that range, retain a conservative age by merging at tail. */
        size_t tail=(clock->head+clock->count-1)%NB_LIVE_QUEUE_SEGMENTS;
        clock->segments[tail].bytes+=added;return;
    }
    size_t tail=(clock->head+clock->count)%NB_LIVE_QUEUE_SEGMENTS;
    clock->segments[tail].queued_at=now_us;
    clock->segments[tail].bytes=added;
    clock->count++;
}

void nb_live_queue_consumed(nb_live_queue_clock_t* clock,size_t consumed,size_t remaining){
    if(clock==NULL)return;
    if(remaining==0){clock->head=0;clock->count=0;return;}
    while(consumed>0&&clock->count>0){
        nb_live_queue_segment_t* segment=&clock->segments[clock->head];
        if(consumed<segment->bytes){segment->bytes-=consumed;consumed=0;}
        else{
            consumed-=segment->bytes;segment->bytes=0;segment->queued_at=0;
            clock->head=(clock->head+1)%NB_LIVE_QUEUE_SEGMENTS;clock->count--;
        }
    }
}

uint64_t nb_live_queue_age_us(const nb_live_queue_clock_t* clock,size_t length,uint64_t now_us){
    if(clock==NULL||length==0||clock->count==0)return 0;
    uint64_t queued_at=clock->segments[clock->head].queued_at;
    return queued_at>0&&now_us>=queued_at?now_us-queued_at:0;
}

nb_live_flow_action_t nb_live_flow_observe(nb_live_flow_runtime_t* runtime,
    nb_flow_class_t flow_class,uint64_t total_c2s,uint64_t total_s2c,uint64_t now_us,
    double* c2s_kbps,double* s2c_kbps){
    if(c2s_kbps)*c2s_kbps=0;
    if(s2c_kbps)*s2c_kbps=0;
    if(runtime==NULL)return NB_LIVE_FLOW_KEEP;
    if(runtime->sampled_at==0){
        runtime->sampled_at=now_us;runtime->sampled_c2s=total_c2s;runtime->sampled_s2c=total_s2c;
        return NB_LIVE_FLOW_KEEP;
    }
    uint64_t elapsed=now_us-runtime->sampled_at;
    if(elapsed<FLOW_SAMPLE_US)return NB_LIVE_FLOW_KEEP;
    uint64_t dc=total_c2s-runtime->sampled_c2s,ds=total_s2c-runtime->sampled_s2c;
    double ck=(double)dc*8000.0/(double)elapsed,sk=(double)ds*8000.0/(double)elapsed;
    if(c2s_kbps)*c2s_kbps=ck;
    if(s2c_kbps)*s2c_kbps=sk;
    runtime->sampled_at=now_us;runtime->sampled_c2s=total_c2s;runtime->sampled_s2c=total_s2c;
    if(flow_class==NB_FLOW_CLASS_CTRL){
        runtime->high_uplink_windows=0;
        if(ck>=128.0||sk>=128.0)runtime->high_ctrl_windows++;
        else runtime->high_ctrl_windows=0;
        return runtime->high_ctrl_windows>=2&&(total_c2s+total_s2c)>=64*KIB?
            NB_LIVE_FLOW_DEMOTE_BULK:NB_LIVE_FLOW_KEEP;
    }
    runtime->high_ctrl_windows=0;
    if(flow_class!=NB_FLOW_CLASS_MEDIA&&ck>=256.0)runtime->high_uplink_windows++;
    else runtime->high_uplink_windows=0;
    return runtime->high_uplink_windows>=2?NB_LIVE_FLOW_PROMOTE_MEDIA:NB_LIVE_FLOW_KEEP;
}

static void sched_refill(nb_live_sched_t* sched,uint64_t now_us){
    if(sched->updated_at==0){sched->updated_at=now_us;sched->ctrl_tokens=CTRL_BURST_BYTES;return;}
    if(now_us<=sched->updated_at)return;
    sched->ctrl_tokens+=(double)(now_us-sched->updated_at)*CTRL_RATE_BYTES_PER_SEC/1000000.0;
    if(sched->ctrl_tokens>CTRL_BURST_BYTES)sched->ctrl_tokens=CTRL_BURST_BYTES;
    sched->updated_at=now_us;
}

size_t nb_live_sched_grant(nb_live_sched_t* sched,nb_flow_class_t flow_class,
    size_t requested,uint64_t now_us,int media_pending){
    if(sched==NULL||requested==0||flow_class!=NB_FLOW_CLASS_CTRL||!media_pending)return requested;
    sched_refill(sched,now_us);
    size_t grant=(size_t)sched->ctrl_tokens;if(grant>requested)grant=requested;
    sched->ctrl_tokens-=(double)grant;return grant;
}

int nb_live_sched_ctrl_ready(nb_live_sched_t* sched,uint64_t now_us){
    if(sched==NULL)return 1;
    sched_refill(sched,now_us);return sched->ctrl_tokens>=1200.0;
}
