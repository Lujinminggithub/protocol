#ifndef NB_SEND_H
#define NB_SEND_H

#include <stddef.h>
#include <stdint.h>

#include "nb_live.h"
#include "nb_ring.h"

int nb_send_append(nb_ring_t* ring,nb_live_queue_clock_t* clock,const uint8_t* data,size_t length,
    size_t limit,uint64_t now_us,size_t* peak);
size_t nb_send_copyout(nb_ring_t* ring,nb_live_queue_clock_t* clock,uint8_t* destination,size_t length);
void nb_send_consume(nb_ring_t* ring,nb_live_queue_clock_t* clock,size_t length);
void nb_send_clear(nb_ring_t* ring,nb_live_queue_clock_t* clock);

#endif
