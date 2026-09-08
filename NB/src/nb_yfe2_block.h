#ifndef NB_YFE2_BLOCK_H
#define NB_YFE2_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "nb_udp.h"
#include "nb_yfe2_wire.h"

#define NB_YFE2_INTERLEAVE 4u
#define NB_YFE2_FLUSH_US 10000ULL

typedef struct {
    uint64_t connection_generation;
    uint32_t session_id;
    uint32_t block_id;
    uint8_t parity_shards;
    uint8_t actual_count;
    uint16_t shard_size;
    nb_yfe2_desc_t desc[NB_YFE2_DATA_SHARDS];
    uint8_t source[NB_YFE2_DATA_SHARDS][NB_UDP_FRAGMENT_PAYLOAD];
} nb_yfe2_encode_job_t;

typedef struct {
    uint64_t connection_generation;
    uint32_t session_id;
    uint32_t block_id;
    uint8_t parity_count;
    struct {size_t length;uint8_t wire[NB_YFE2_MAX_PARITY_WIRE];} parity[NB_YFE2_BURST_PARITY];
} nb_yfe2_encode_result_t;

typedef struct {int active;uint64_t first_at;nb_yfe2_encode_job_t job;} nb_yfe2_tx_slot_t;
typedef struct {
    uint64_t connection_generation;
    uint32_t session_id;
    uint32_t next_block_id;
    uint8_t next_slot;
    uint8_t parity_shards;
    nb_yfe2_tx_slot_t slots[NB_YFE2_INTERLEAVE];
} nb_yfe2_tx_t;

void nb_yfe2_tx_init(nb_yfe2_tx_t* state,uint64_t generation,uint32_t session_id);
void nb_yfe2_tx_set_parity(nb_yfe2_tx_t* state,uint8_t parity_shards);
size_t nb_yfe2_tx_slot_count(const nb_yfe2_tx_t* state,size_t slot);
uint64_t nb_yfe2_tx_next_deadline(const nb_yfe2_tx_t* state);
int nb_yfe2_tx_add(nb_yfe2_tx_t* state,const nb_udp_wire_view_t* source,
    uint64_t now_us,uint8_t parity_shards,nb_yfe2_encode_job_t* sealed);
int nb_yfe2_tx_flush_due(nb_yfe2_tx_t* state,uint64_t now_us,nb_yfe2_encode_job_t* sealed);
int nb_yfe2_encode_job_run(const nb_yfe2_encode_job_t* job,nb_yfe2_encode_result_t* result);

#endif
