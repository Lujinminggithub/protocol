#ifndef NB_CONNECT_H
#define NB_CONNECT_H

#include <stdint.h>

uint64_t nb_connect_deadline_us(uint64_t started_at_us);
int nb_connect_expired(uint64_t started_at_us, uint64_t now_us);

#endif
