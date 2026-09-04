#include "nb_connect.h"

#define NB_CONNECT_TIMEOUT_US 5000000ULL

uint64_t nb_connect_deadline_us(uint64_t started_at_us)
{
    if (started_at_us == UINT64_MAX || started_at_us > UINT64_MAX - NB_CONNECT_TIMEOUT_US) {
        return UINT64_MAX;
    }
    return started_at_us + NB_CONNECT_TIMEOUT_US;
}

int nb_connect_expired(uint64_t started_at_us, uint64_t now_us)
{
    uint64_t deadline_us;

    if (started_at_us == UINT64_MAX) {
        return 0;
    }
    deadline_us = nb_connect_deadline_us(started_at_us);
    return now_us >= deadline_us;
}
