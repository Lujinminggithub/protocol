#ifndef NB_UDP_QUEUE_H
#define NB_UDP_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define NB_UDP_QUEUE_RECORD_HEADER 10u

uint64_t nb_udp_queue_oldest_age_us(const uint8_t* queue,size_t length,uint64_t now_us);

int nb_udp_queue_drop_oldest_packet(uint8_t* queue,size_t* length,
    size_t* removed_bytes,uint64_t* dropped_packets);

#endif
