#ifndef NB_YFE2_ADAPTIVE_H
#define NB_YFE2_ADAPTIVE_H

#include <stdint.h>

#define NB_YFE2_HOLD_US 5000000ULL
enum { NB_YFE2_MODE_BASELINE=0,NB_YFE2_MODE_BURST=1 };
enum { NB_YFE2_IGNORE_LOCAL_DROP=1u<<0,NB_YFE2_IGNORE_SCHEDULER=1u<<1,
    NB_YFE2_IGNORE_SEND_QUEUE=1u<<2,NB_YFE2_IGNORE_PMTU=1u<<3,
    NB_YFE2_IGNORE_APP_LIMITED=1u<<4,NB_YFE2_IGNORE_RATE_CAP=1u<<5,
    NB_YFE2_IGNORE_PROBE=1u<<6,NB_YFE2_IGNORE_REASON_COUNT=7 };

typedef struct {
    uint64_t generation,sent,declared_lost,spurious_lost;
    uint64_t local_drops,scheduler_expired,send_queue_full,pmtu_fallbacks;
    uint32_t invalid_reasons;
} nb_yfe2_loss_sample_t;
typedef struct {
    int initialized;
    int mode;
    uint64_t burst_until_us;
    uint64_t effective_physical_loss;
    uint64_t transitions;
    uint64_t last_trigger_us;
    uint64_t ignored_samples[NB_YFE2_IGNORE_REASON_COUNT];
    nb_yfe2_loss_sample_t previous;
} nb_yfe2_adaptive_t;

void nb_yfe2_adaptive_init(nb_yfe2_adaptive_t* state);
int nb_yfe2_adaptive_update(nb_yfe2_adaptive_t* state,const nb_yfe2_loss_sample_t* sample,uint64_t now_us);

#endif
