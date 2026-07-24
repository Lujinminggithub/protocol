#ifndef NB_UDP_LIFECYCLE_H
#define NB_UDP_LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>

#include "nb_udp.h"

#define NB_UDP_LIFECYCLE_CAP 256
#define NB_UDP_CLOSE_RETRY_US 250000ULL
#define NB_UDP_CLOSE_ATTEMPTS 5

typedef struct {
    uintptr_t link;
    uint32_t session_id;
    uint64_t next_send_at;
    uint8_t wire[NB_UDP_ROUTE_MAX + 32];
    uint16_t wire_length;
    uint8_t type;
    uint8_t attempts;
    uint8_t active;
    uint8_t ready_marked;
} nb_udp_lifecycle_item_t;

typedef struct {
    nb_udp_lifecycle_item_t items[NB_UDP_LIFECYCLE_CAP];
} nb_udp_lifecycle_t;

void nb_udp_lifecycle_init(nb_udp_lifecycle_t* state);
int nb_udp_lifecycle_schedule(nb_udp_lifecycle_t* state, uintptr_t link, uint8_t type,
    uint32_t session_id, const char* route, uint64_t now_us);
int nb_udp_lifecycle_next(nb_udp_lifecycle_t* state, uintptr_t link, uint64_t now_us,
    const uint8_t** wire, size_t* wire_length, size_t* token);
void nb_udp_lifecycle_sent(nb_udp_lifecycle_t* state, size_t token, uint64_t now_us);
int nb_udp_lifecycle_ack(nb_udp_lifecycle_t* state, uintptr_t link, uint32_t session_id);
uintptr_t nb_udp_lifecycle_mark_due(nb_udp_lifecycle_t* state, uint64_t now_us);
uint64_t nb_udp_lifecycle_next_deadline(const nb_udp_lifecycle_t* state);
void nb_udp_lifecycle_forget_link(nb_udp_lifecycle_t* state, uintptr_t link);
size_t nb_udp_lifecycle_count(const nb_udp_lifecycle_t* state);

#endif
