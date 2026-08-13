#include "nb_transport_seed.h"

#include <string.h>

#define NB_SEED_MIN_RTT_US 1000ULL
#define NB_SEED_MAX_RTT_US 60000000ULL
#define NB_SEED_MIN_CWIN (256ULL*1024ULL)
#define NB_SEED_MAX_CWIN (64ULL*1024ULL*1024ULL)
#define NB_SEED_CWIN_QUANTUM (64ULL*1024ULL)

static uint64_t ceil_quantum(uint64_t value,uint64_t quantum){
    if(value>UINT64_MAX-(quantum-1))return UINT64_MAX;
    return ((value+quantum-1)/quantum)*quantum;
}

static uint64_t scale_cwin(uint64_t cwin,uint64_t numerator,uint64_t denominator){
    long double scaled=(long double)cwin*(long double)numerator/(long double)denominator;
    uint64_t value=scaled>=(long double)UINT64_MAX?UINT64_MAX:(uint64_t)scaled;
    if((long double)value<scaled&&value<UINT64_MAX)value++;
    value=ceil_quantum(value,NB_SEED_CWIN_QUANTUM);
    if(value<NB_SEED_MIN_CWIN)value=NB_SEED_MIN_CWIN;
    if(value>NB_SEED_MAX_CWIN)value=NB_SEED_MAX_CWIN;
    return value;
}

int nb_transport_seed_plan(const nb_transport_link_profile_t* link,
    uint64_t observed_rtt_us,nb_transport_seed_plan_t* plan){
    if(link==NULL||plan==NULL)return -1;
    memset(plan,0,sizeof(*plan));
    if(strcmp(link->cc,"bbr")||link->target_rate_bps==0||link->seed_rtt_us==0||
        link->startup_cwin_bytes==0)return 0;
    plan->configured=1;
    plan->rtt_us=link->seed_rtt_us;
    plan->cwin_bytes=link->startup_cwin_bytes;
    if(observed_rtt_us>=NB_SEED_MIN_RTT_US&&observed_rtt_us<=NB_SEED_MAX_RTT_US){
        plan->rtt_us=observed_rtt_us;
        plan->cwin_bytes=scale_cwin(link->startup_cwin_bytes,observed_rtt_us,link->seed_rtt_us);
    }
    return 0;
}

int nb_transport_seed_should_refresh(int configured,int applied,
    uint64_t current_rtt_us,uint64_t observed_rtt_us){
    if(!configured)return 0;
    if(observed_rtt_us<NB_SEED_MIN_RTT_US||observed_rtt_us>NB_SEED_MAX_RTT_US)return 0;
    if(current_rtt_us==observed_rtt_us)return 0;
    if(!applied)return 1;
    uint64_t delta=current_rtt_us>observed_rtt_us?
        current_rtt_us-observed_rtt_us:observed_rtt_us-current_rtt_us;
    uint64_t threshold=current_rtt_us/8;
    if(threshold<NB_SEED_MIN_RTT_US)threshold=NB_SEED_MIN_RTT_US;
    return delta>=threshold;
}
