#ifndef NB_YFE2_RX_H
#define NB_YFE2_RX_H

#include <stddef.h>
#include <stdint.h>

#include "nb_udp.h"
#include "nb_yfe2_block.h"
#include "nb_yfe2_wire.h"

#define NB_YFE2_RX_FLOW_BLOCKS 32u
#define NB_YFE2_RX_FLOW_BYTES (2u*1024u*1024u)
#define NB_YFE2_RX_GLOBAL_BLOCKS 256u
#define NB_YFE2_RX_GLOBAL_BYTES (32u*1024u*1024u)
#define NB_YFE2_RECOVERY_DEADLINE_US 150000ULL
#define NB_YFE2_RECOVERED_WIRE_MAX (NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32u)

typedef struct nb_yfe2_rx nb_yfe2_rx_t;
typedef struct {size_t length;uint8_t wire[NB_YFE2_RECOVERED_WIRE_MAX];} nb_yfe2_recovered_t;
typedef struct {uint64_t recovered,unrecoverable,duplicate,corrupt,evicted,memory_high,decode_ns;} nb_yfe2_rx_metrics_t;

nb_yfe2_rx_t* nb_yfe2_rx_create(void);
void nb_yfe2_rx_destroy(nb_yfe2_rx_t* state);
int nb_yfe2_rx_note_source(nb_yfe2_rx_t* state,uint64_t generation,
    const nb_udp_wire_view_t* source,uint64_t now_us);
int nb_yfe2_rx_add_parity(nb_yfe2_rx_t* state,uint64_t generation,
    const nb_yfe2_parity_view_t* parity,const char* route,uint16_t route_length,
    uint64_t now_us,nb_yfe2_recovered_t* output,size_t output_cap,size_t* output_count);
size_t nb_yfe2_rx_expire(nb_yfe2_rx_t* state,uint64_t now_us,nb_yfe2_rx_metrics_t* metrics);
uint64_t nb_yfe2_rx_next_deadline(const nb_yfe2_rx_t* state);
void nb_yfe2_rx_snapshot(const nb_yfe2_rx_t* state,nb_yfe2_rx_metrics_t* out);

#endif
