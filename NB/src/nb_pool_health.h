#ifndef NB_POOL_HEALTH_H
#define NB_POOL_HEALTH_H

#include <stdint.h>

typedef struct {
    uint64_t degraded_since_us;
    uint64_t last_retired_us;
    uint64_t last_progress_us;
    uint64_t last_delivered;
    uint64_t retire_count;
    uint64_t suppressed_count;
    int quarantined;
} nb_pool_health_t;

typedef struct {
    uint64_t queue_age_threshold_us;
    uint64_t hold_us;
    uint64_t progress_grace_us;
    uint64_t cooldown_us;
} nb_pool_health_config_t;

typedef enum {
    NB_POOL_RECOVERY_NONE = 0,
    NB_POOL_RECOVERY_START_REPLACEMENT,
    NB_POOL_RECOVERY_PROMOTE_REPLACEMENT,
    NB_POOL_RECOVERY_CLOSE_REPLACEMENT,
} nb_pool_recovery_action_t;

int nb_pool_health_update(nb_pool_health_t* state, const nb_pool_health_config_t* config,
    uint64_t now_us, int eligible, uint64_t queue_age_us, int transport_blocked,
    uint64_t delivered);
int nb_pool_health_retire_now(nb_pool_health_t* state,const nb_pool_health_config_t* config,
    uint64_t now_us);
nb_pool_recovery_action_t nb_pool_health_recovery_action(const nb_pool_health_t* state,
    int has_replacement, int replacement_ready, int has_draining);

#endif
