#ifndef NB_POOL_H
#define NB_POOL_H

#include <stdint.h>
#include <sys/socket.h>
#include <picoquic.h>
#include "nb_pool_health.h"
#include "nb_pmtu.h"

#define NB_POOL_SIZE 1

typedef struct cnx_pool {
    picoquic_cnx_t* cnx[NB_POOL_SIZE];
    picoquic_cnx_t* replacement_cnx[NB_POOL_SIZE];
    picoquic_cnx_t* draining_cnx[NB_POOL_SIZE];
    uint8_t replacement_closing[NB_POOL_SIZE];
    uint8_t draining_closing[NB_POOL_SIZE];
    uint8_t generation_refresh[NB_POOL_SIZE];
    uint64_t next_sid[NB_POOL_SIZE];
    struct sockaddr_storage addr;
    int configured;
    int rr;
    int rr_lat;
    double recent_loss[NB_POOL_SIZE];
    uint64_t recent_rtt[NB_POOL_SIZE];
    uint64_t recent_rtt_max[NB_POOL_SIZE];
    uint64_t recent_sent[NB_POOL_SIZE];
    uint64_t last_sent_total[NB_POOL_SIZE];
    uint64_t last_lost_total[NB_POOL_SIZE];
    uint64_t last_timer_total[NB_POOL_SIZE];
    uint64_t last_spurious_total[NB_POOL_SIZE];
    uint64_t last_retrans_total[NB_POOL_SIZE];
    uint64_t last_preempt_total[NB_POOL_SIZE];
    uint64_t recent_rtt_var[NB_POOL_SIZE];
    uint64_t recent_ts[NB_POOL_SIZE];
    nb_pool_health_t health[NB_POOL_SIZE];
    nb_pmtu_state_t pmtu[NB_POOL_SIZE];
    int fec_latched;
} cnx_pool_t;

void nb_pool_slot_reset_quality(cnx_pool_t* pool, int idx);
int nb_pool_health_evaluate(cnx_pool_t* pool, int idx,
    const nb_pool_health_config_t* config, uint64_t now_us, int eligible,
    uint64_t queue_age_us, int transport_blocked, uint64_t delivered,
    uint64_t* suppressed_delta);

#endif
