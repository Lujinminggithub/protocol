#ifndef NB_UDP_QUEUE_H
#define NB_UDP_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define NB_UDP_QUEUE_RECORD_HEADER 10u
#define NB_UDP_QUEUE_MAX_BYTES (1024u*1024u)
#define NB_UDP_QUEUE_MIN_RECORD_BYTES (NB_UDP_QUEUE_RECORD_HEADER+1u)
#define NB_UDP_QUEUE_MAX_RECORDS (NB_UDP_QUEUE_MAX_BYTES/NB_UDP_QUEUE_MIN_RECORD_BYTES)
#define NB_UDP_QUEUE_DROP_WINDOW_BITS 64u
#define NB_UDP_QUEUE_DROP_WINDOW_SLOTS 2u

typedef struct {
    uint8_t direction;
    uint32_t session_id;
    uint32_t sequence;
} nb_udp_queue_packet_key_t;

typedef struct {
    uint8_t direction;
    uint32_t session_id;
    uint32_t highest_sequence;
    uint64_t dropped_sequences;
} nb_udp_queue_drop_window_t;

typedef struct {
    nb_udp_queue_drop_window_t windows[NB_UDP_QUEUE_DROP_WINDOW_SLOTS];
} nb_udp_queue_drop_history_t;

typedef void (*nb_udp_queue_drop_observer_t)(void* context,
    const nb_udp_queue_packet_key_t* key);

uint64_t nb_udp_queue_oldest_age_us(const uint8_t* queue,size_t length,uint64_t now_us);
int nb_udp_queue_oldest_packet_key(const uint8_t* queue,size_t length,
    nb_udp_queue_packet_key_t* key);
void nb_udp_queue_drop_history_note(nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us);
int nb_udp_queue_drop_history_contains(const nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us,uint64_t max_age_us);
int nb_udp_queue_trim_to_limit(uint8_t* queue,size_t* length,size_t need,size_t queue_limit,
    nb_udp_queue_drop_observer_t observer,void* observer_context,size_t* removed_bytes,
    uint64_t* dropped_packets);
#ifdef NB_NODE_QUEUE_TEST
int nb_udp_queue_trim_to_limit_forced_hash_test(uint8_t* queue,size_t* length,size_t need,
    size_t queue_limit,size_t* removed_bytes,uint64_t* dropped_packets);
#endif

int nb_udp_queue_drop_oldest_packet(uint8_t* queue,size_t* length,
    size_t* removed_bytes,uint64_t* dropped_packets);

#endif
