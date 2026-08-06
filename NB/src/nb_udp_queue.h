#ifndef NB_UDP_QUEUE_H
#define NB_UDP_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define NB_UDP_QUEUE_RECORD_HEADER 10u

int nb_udp_queue_drop_oldest_packet(uint8_t* queue,size_t* length,
    size_t* removed_bytes,uint64_t* dropped_packets);

#endif
