#include "nb_pool_health.h"

int nb_pool_health_retire_now(nb_pool_health_t* state,const nb_pool_health_config_t* config,
    uint64_t now_us){
    if(state==0||config==0)return 0;
    if(state->last_retired_us&&now_us>=state->last_retired_us&&
        now_us-state->last_retired_us<config->cooldown_us){state->suppressed_count++;return 0;}
    state->degraded_since_us=0;state->last_retired_us=now_us;state->retire_count++;
    state->promotion_ready=1;return 1;
}

int nb_pool_health_update(nb_pool_health_t* state, const nb_pool_health_config_t* config,
    uint64_t now_us, int eligible, uint64_t queue_age_us, int transport_blocked,
    uint64_t delivered){
    if(state==0||config==0)return 0;
    if(state->last_progress_us==0||state->last_delivered!=delivered){
        state->last_delivered=delivered;state->last_progress_us=now_us;
    }
    int progress_stalled=now_us>=state->last_progress_us&&
        now_us-state->last_progress_us>=config->progress_grace_us;
    int degraded=eligible&&transport_blocked&&
        queue_age_us>=config->queue_age_threshold_us;
    if(!degraded){state->degraded_since_us=0;state->quarantined=0;
        state->promotion_ready=0;return 0;}
    if(state->degraded_since_us==0){state->degraded_since_us=now_us;return 0;}
    if(!progress_stalled){state->quarantined=0;state->promotion_ready=0;return 0;}
    state->quarantined=1;
    if(now_us<state->degraded_since_us||now_us-state->degraded_since_us<config->hold_us)return 0;
    state->degraded_since_us=0;return nb_pool_health_retire_now(state,config,now_us);
}

nb_pool_recovery_action_t nb_pool_health_recovery_action(const nb_pool_health_t* state,
    int has_replacement,int replacement_ready,int has_draining){
    if(state==0)return NB_POOL_RECOVERY_NONE;
    if(!state->quarantined)return has_replacement
        ?NB_POOL_RECOVERY_CLOSE_REPLACEMENT:NB_POOL_RECOVERY_NONE;
    if(!has_replacement)return NB_POOL_RECOVERY_START_REPLACEMENT;
    if(state->promotion_ready&&replacement_ready&&!has_draining)
        return NB_POOL_RECOVERY_PROMOTE_REPLACEMENT;
    return NB_POOL_RECOVERY_NONE;
}

int nb_pool_route_pick(const unsigned char* available,const nb_pool_health_t* states,
    int count,int start,int avoid){
    if(available==0||states==0||count<=0)return -1;
    if(start<0)start=0;
    start%=count;
    for(int pass=0;pass<2;pass++)for(int offset=0;offset<count;offset++){
        int index=(start+offset)%count;
        if(index==avoid||!available[index])continue;
        if((states[index].quarantined?1:0)==pass)return index;
    }
    return -1;
}
