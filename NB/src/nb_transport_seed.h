#ifndef NB_TRANSPORT_SEED_H
#define NB_TRANSPORT_SEED_H

#include <stdint.h>

#include "nb_transport_profile.h"

typedef struct {
    uint64_t rtt_us;
    uint64_t cwin_bytes;
    int configured;
} nb_transport_seed_plan_t;

int nb_transport_seed_plan(const nb_transport_link_profile_t* link,
    uint64_t observed_rtt_us,nb_transport_seed_plan_t* plan);
int nb_transport_seed_should_refresh(int configured,int applied,
    uint64_t current_rtt_us,uint64_t observed_rtt_us);

#endif
