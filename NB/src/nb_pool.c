#include "nb_pool.h"

void nb_pool_slot_reset_quality(cnx_pool_t* pool, int idx){
    if(pool==0||idx<0||idx>=NB_POOL_SIZE)return;
    pool->recent_loss[idx]=0;pool->recent_rtt[idx]=0;pool->recent_rtt_max[idx]=0;
    pool->recent_sent[idx]=0;pool->last_sent_total[idx]=0;pool->last_lost_total[idx]=0;
    pool->last_timer_total[idx]=0;pool->last_spurious_total[idx]=0;
    pool->last_retrans_total[idx]=0;pool->last_preempt_total[idx]=0;
    pool->recent_rtt_var[idx]=0;pool->recent_ts[idx]=0;
    nb_pmtu_init(&pool->pmtu[idx],NB_PMTU_DEFAULT_FLOOR,NB_PMTU_DEFAULT_CEILING);
}

int nb_pool_health_evaluate(cnx_pool_t* pool, int idx,
    const nb_pool_health_config_t* config, uint64_t now_us, int eligible,
    uint64_t queue_age_us, int transport_blocked, uint64_t delivered,
    uint64_t* suppressed_delta){
    if(suppressed_delta)*suppressed_delta=0;
    if(pool==0||idx<0||idx>=NB_POOL_SIZE)return 0;
    uint64_t before=pool->health[idx].suppressed_count;
    int retire=nb_pool_health_update(&pool->health[idx],config,now_us,eligible,
        queue_age_us,transport_blocked,delivered);
    if(suppressed_delta)*suppressed_delta=pool->health[idx].suppressed_count-before;
    return retire;
}
