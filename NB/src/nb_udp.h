#ifndef NB_UDP_H
#define NB_UDP_H

#include <stddef.h>
#include <stdint.h>

#define NB_UDP_MAGIC 0x4e425544u /* NBUD */
#define NB_UDP_VERSION 1
#define NB_UDP_TYPE_C2S 1
#define NB_UDP_TYPE_S2C 2
#define NB_UDP_TYPE_CLOSE 3
#define NB_UDP_TYPE_CLOSE_ACK 4
#define NB_UDP_ROUTE_MAX 300
#define NB_UDP_FRAGMENT_PAYLOAD 1000
#define NB_UDP_MAX_PAYLOAD 65507
#define NB_UDP_REASSEMBLY_SLOTS 8
#define NB_UDP_REASSEMBLY_TIMEOUT_US 5000000ULL
/* Some mobile proxy clients close the SOCKS UDP control TCP connection after
 * setup while continuing to use the negotiated UDP relay. Keep the association
 * while either UDP direction is active, then reclaim it on the normal idle
 * horizon. */
#define NB_UDP_CONTROL_GRACE_US 120000000ULL

typedef struct {
    uint8_t type;
    uint32_t session_id;
    uint32_t sequence;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t total_length;
    const uint8_t* route;
    uint16_t route_length;
    const uint8_t* payload;
    uint16_t payload_length;
} nb_udp_wire_view_t;

typedef struct {
    uint32_t sequence;
    uint64_t updated_at;
    uint16_t total_length;
    uint16_t fragment_count;
    uint16_t received_count;
    uint64_t received[2];
    uint8_t* data;
    char route[NB_UDP_ROUTE_MAX];
    uint16_t route_length;
    int active;
} nb_udp_reassembly_slot_t;

typedef struct {
    nb_udp_reassembly_slot_t slots[NB_UDP_REASSEMBLY_SLOTS];
} nb_udp_reassembly_t;

typedef struct {
    const uint8_t* payload;
    uint16_t payload_length;
    const char* route;
    uint16_t route_length;
    uint32_t sequence;
} nb_udp_reassembled_t;

int nb_udp_wire_encode(uint8_t* out, size_t cap, uint8_t type,
    uint32_t session_id, uint32_t sequence, uint16_t fragment_index,
    uint16_t fragment_count, uint16_t total_length, const char* route,
    uint16_t route_length, const uint8_t* payload, uint16_t payload_length);
int nb_udp_wire_decode(const uint8_t* data, size_t length, nb_udp_wire_view_t* out);
int nb_udp_wire_type_valid(uint8_t type);
uint16_t nb_udp_fragment_count(size_t payload_length);

void nb_udp_reassembly_init(nb_udp_reassembly_t* state);
void nb_udp_reassembly_dispose(nb_udp_reassembly_t* state);
int nb_udp_reassembly_feed(nb_udp_reassembly_t* state, const nb_udp_wire_view_t* fragment,
    uint64_t now_us, nb_udp_reassembled_t* out);
int nb_udp_control_grace_expired(uint64_t control_closed_at, uint64_t last_active,
    uint64_t now_us, uint64_t grace_us);

int nb_socks_udp_parse(const uint8_t* data, size_t length, char* host, size_t host_cap,
    int* port, const uint8_t** payload, size_t* payload_length);
int nb_socks_udp_encode(uint8_t* out, size_t cap, const char* host, int port,
    const uint8_t* payload, size_t payload_length);

#endif
