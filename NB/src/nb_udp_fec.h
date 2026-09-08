#ifndef NB_UDP_FEC_H
#define NB_UDP_FEC_H

#include <stddef.h>
#include <stdint.h>

#include "nb_udp.h"

#define NB_UDP_FEC_MAGIC 0x4e425546u /* NBUF */
#define NB_UDP_FEC_VERSION 2u
#define NB_UDP_FEC_MAX_K 8u
#define NB_UDP_FEC_MAX_R 4u
#define NB_UDP_FEC_DEFAULT_R 2u
#define NB_UDP_FEC_DEFAULT_K 8u
#define NB_UDP_FEC_DEFAULT_HOLD_US 2000u

/* Media UDP FEC uses systematic Reed-Solomon over GF(256): K source shards
 * plus NB_UDP_FEC_MAX_R independent repair shards per block. */

typedef struct nb_udp_fec_tx nb_udp_fec_tx_t;
typedef struct nb_udp_fec_rx nb_udp_fec_rx_t;
typedef struct {
    int active;
    uint8_t repair_count;
    uint64_t changed_at;
} nb_udp_fec_adaptive_t;

nb_udp_fec_tx_t* nb_udp_fec_tx_create(uint8_t k,uint64_t hold_us);
nb_udp_fec_tx_t* nb_udp_fec_tx_create_ex(uint8_t k,uint8_t repair_count,uint64_t hold_us);
void nb_udp_fec_tx_destroy(nb_udp_fec_tx_t* state);
uint64_t nb_udp_fec_tx_next_deadline(const nb_udp_fec_tx_t* state);
int nb_udp_fec_tx_feed(nb_udp_fec_tx_t* state,const nb_udp_wire_view_t* source,
    uint64_t now_us,uint8_t* repair,size_t repair_cap);
int nb_udp_fec_tx_flush_due(nb_udp_fec_tx_t* state,uint64_t now_us,
    uint8_t* repair,size_t repair_cap);
int nb_udp_fec_tx_next_repair(nb_udp_fec_tx_t* state,uint8_t* repair,size_t repair_cap);
int nb_udp_fec_tx_set_repair_count(nb_udp_fec_tx_t* state,uint8_t repair_count);

nb_udp_fec_rx_t* nb_udp_fec_rx_create(void);
void nb_udp_fec_rx_destroy(nb_udp_fec_rx_t* state);
int nb_udp_fec_rx_note(nb_udp_fec_rx_t* state,const nb_udp_wire_view_t* source,uint64_t now_us);
int nb_udp_fec_rx_seen(const nb_udp_fec_rx_t* state,const nb_udp_wire_view_t* source);
int nb_udp_fec_rx_recover(nb_udp_fec_rx_t* state,const uint8_t* repair,size_t repair_len,
    uint64_t now_us,uint8_t* recovered,size_t recovered_cap);
int nb_udp_fec_peek(const uint8_t* data,size_t length,uint32_t* session_id,uint8_t* direction);
int nb_udp_fec_adaptive_update(nb_udp_fec_adaptive_t* state,double loss_pct,
    uint64_t jitter_us,int sample_valid,uint64_t now_us);

#endif
