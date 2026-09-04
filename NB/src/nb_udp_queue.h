#ifndef NB_UDP_QUEUE_H
#define NB_UDP_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define NB_UDP_QUEUE_RECORD_HEADER 10u
#define NB_UDP_QUEUE_DROPPED_HISTORY 32u

typedef struct {
    uint8_t direction;
    uint32_t session_id;
    uint32_t sequence;
} nb_udp_queue_packet_key_t;

typedef struct {
    nb_udp_queue_packet_key_t keys[NB_UDP_QUEUE_DROPPED_HISTORY];
    uint64_t dropped_at[NB_UDP_QUEUE_DROPPED_HISTORY];
    size_t next;
} nb_udp_queue_drop_history_t;

uint64_t nb_udp_queue_oldest_age_us(const uint8_t* queue,size_t length,uint64_t now_us);
int nb_udp_queue_oldest_packet_key(const uint8_t* queue,size_t length,
    nb_udp_queue_packet_key_t* key);
void nb_udp_queue_drop_history_note(nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us);
int nb_udp_queue_drop_history_contains(const nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us,uint64_t max_age_us);

int nb_udp_queue_drop_oldest_packet(uint8_t* queue,size_t* length,
    size_t* removed_bytes,uint64_t* dropped_packets);

#endif
