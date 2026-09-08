#ifndef NB_YFE2_WIRE_H
#define NB_YFE2_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "nb_yfe2_profile.h"

#define NB_YFE2_WIRE_HEADER 24u
#define NB_YFE2_WIRE_DESC 12u
#define NB_YFE2_DATA_SHARDS 16u
#define NB_YFE2_BASE_PARITY 1u
#define NB_YFE2_BURST_PARITY 3u
#define NB_YFE2_FLAG_BURST 0x01u
#define NB_YFE2_DIRECTION_C2S 1u
#define NB_YFE2_DIRECTION_S2C 2u
#define NB_YFE2_MAX_PARITY_WIRE 1408u

typedef enum { NB_YFE2_WIRE_OK=0,NB_YFE2_WIRE_ARGUMENT=-1,NB_YFE2_WIRE_MAGIC=-2,
    NB_YFE2_WIRE_VERSION=-3,NB_YFE2_WIRE_FLAGS=-4,NB_YFE2_WIRE_PROFILE=-5,
    NB_YFE2_WIRE_DIRECTION=-6,NB_YFE2_WIRE_SHARDS=-7,NB_YFE2_WIRE_RESERVED=-8,
    NB_YFE2_WIRE_LENGTH=-9,NB_YFE2_WIRE_DESCRIPTOR=-10 } nb_yfe2_wire_error_t;

typedef struct {uint32_t sequence;uint16_t fragment_index,fragment_count,total_length,payload_length;} nb_yfe2_desc_t;
typedef struct {
    uint8_t flags,direction,shard_index,data_shards,parity_shards,actual_data_count;
    uint16_t profile_id,shard_size;
    uint32_t session_id,block_id;
    const nb_yfe2_desc_t* desc;
    const uint8_t* body;
} nb_yfe2_parity_t;
typedef struct {
    uint8_t flags,direction,shard_index,data_shards,parity_shards,actual_data_count;
    uint16_t profile_id,shard_size;
    uint32_t session_id,block_id;
    nb_yfe2_desc_t desc[NB_YFE2_DATA_SHARDS];
    const uint8_t* body;
    size_t body_length;
} nb_yfe2_parity_view_t;

int nb_yfe2_wire_encode(uint8_t* out,size_t cap,const nb_yfe2_parity_t* value);
nb_yfe2_wire_error_t nb_yfe2_wire_decode(const uint8_t* wire,size_t length,nb_yfe2_parity_view_t* out);

#endif
