#ifndef NB_PMTU_H
#define NB_PMTU_H

#include <stddef.h>
#include <stdint.h>

#define NB_PMTU_DEFAULT_FLOOR 1200U
#define NB_PMTU_DEFAULT_CEILING 1500U

typedef struct nb_pmtu_state {
    size_t floor;
    size_t ceiling;
    size_t confirmed;
    size_t candidate;
    unsigned confirmations;
    uint64_t cooldown_until_us;
    uint64_t promotions;
    uint64_t fallbacks;
} nb_pmtu_state_t;

void nb_pmtu_init(nb_pmtu_state_t* state, size_t floor, size_t ceiling);
int nb_pmtu_observe(nb_pmtu_state_t* state, size_t path_mtu,
    uint64_t timer_losses, uint64_t spurious_losses, uint64_t now_us);
size_t nb_pmtu_effective(const nb_pmtu_state_t* state);

#endif
