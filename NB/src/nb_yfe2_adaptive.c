#include "nb_yfe2_adaptive.h"

#include <string.h>

void nb_yfe2_adaptive_init(nb_yfe2_adaptive_t* state){if(state)memset(state,0,sizeof(*state));}
static int regressed(const nb_yfe2_loss_sample_t* a,const nb_yfe2_loss_sample_t* b){return
    a->generation!=b->generation||a->sent<b->sent||a->declared_lost<b->declared_lost||
    a->spurious_lost<b->spurious_lost||a->local_drops<b->local_drops||
    a->scheduler_expired<b->scheduler_expired||a->send_queue_full<b->send_queue_full||
    a->pmtu_fallbacks<b->pmtu_fallbacks;}
int nb_yfe2_adaptive_update(nb_yfe2_adaptive_t* state,const nb_yfe2_loss_sample_t* sample,uint64_t now_us){
    if(state==NULL||sample==NULL)return NB_YFE2_MODE_BASELINE;
    if(!state->initialized||regressed(sample,&state->previous)){
        state->initialized=1;state->mode=NB_YFE2_MODE_BASELINE;state->burst_until_us=0;
        state->previous=*sample;return state->mode;
    }
    uint64_t declared=sample->declared_lost-state->previous.declared_lost;
    uint64_t spurious=sample->spurious_lost-state->previous.spurious_lost;
    uint64_t sent=sample->sent-state->previous.sent;
    uint32_t invalid=sample->invalid_reasons;
    if(sent>=NB_YFE2_APP_LIMITED_MIN_PACKETS)invalid&=~NB_YFE2_IGNORE_APP_LIMITED;
    if(sample->local_drops>state->previous.local_drops)invalid|=NB_YFE2_IGNORE_LOCAL_DROP;
    if(sample->scheduler_expired>state->previous.scheduler_expired)invalid|=NB_YFE2_IGNORE_SCHEDULER;
    if(sample->send_queue_full>state->previous.send_queue_full)invalid|=NB_YFE2_IGNORE_SEND_QUEUE;
    if(sample->pmtu_fallbacks>state->previous.pmtu_fallbacks)invalid|=NB_YFE2_IGNORE_PMTU;
    state->previous=*sample;
    if(invalid){for(unsigned i=0;i<NB_YFE2_IGNORE_REASON_COUNT;i++)if(invalid&(1u<<i))state->ignored_samples[i]++;}
    else {uint64_t effective=declared>spurious?declared-spurious:0;if(effective){
        int was_burst=state->mode==NB_YFE2_MODE_BURST&&now_us<state->burst_until_us;
        state->effective_physical_loss+=effective;state->mode=NB_YFE2_MODE_BURST;
        state->burst_until_us=now_us>UINT64_MAX-NB_YFE2_HOLD_US?UINT64_MAX:now_us+NB_YFE2_HOLD_US;
        state->last_trigger_us=now_us;if(!was_burst)state->transitions++;}}
    if(state->mode==NB_YFE2_MODE_BURST&&now_us>=state->burst_until_us)state->mode=NB_YFE2_MODE_BASELINE;
    return state->mode;
}
