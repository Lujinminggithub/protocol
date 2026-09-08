#ifndef NB_YFE2_NODE_H
#define NB_YFE2_NODE_H

#include <stddef.h>
#include <stdint.h>

#include "nb_udp.h"
#include "nb_yfe2_block.h"
#include "nb_yfe2_negotiation.h"

#define NB_YFE2_CONTROL_HOST "nb-fec-capability.internal"
#define NB_YFE2_CONTROL_ROUTE "T:nb-fec-capability.internal:9"
#define NB_YFE2_CONTROL_PORT 9
#define NB_PRIO_FEC 12

enum {
    NB_YFE2_ROLE_ENTRY=0,
    NB_YFE2_ROLE_MIDDLE=1,
    NB_YFE2_ROLE_EXIT=2
};
enum {
    NB_YFE2_START_NEGOTIATE=0,
    NB_YFE2_START_WAIT=1,
    NB_YFE2_START_FALLBACK=2
};
enum {
    NB_YFE2_PREPARE_SEND=0,
    NB_YFE2_PREPARE_WAIT=1,
    NB_YFE2_PREPARE_FALLBACK=2
};

typedef struct {
    int (*record_source)(void* context,const nb_udp_wire_view_t* source,uint64_t now_us,
        nb_yfe2_encode_job_t* sealed_job);
    int (*submit_job)(void* context,const nb_yfe2_encode_job_t* job);
    int (*queue_parity)(void* context,const uint8_t* wire,size_t length,int priority);
    void* context;
} nb_yfe2_node_ops_t;

int nb_yfe2_node_on_original_queued(nb_yfe2_node_ops_t* ops,
    const nb_udp_wire_view_t* source,uint64_t now_us);
int nb_yfe2_node_drain_result(nb_yfe2_node_ops_t* ops,
    const nb_yfe2_encode_result_t* result,uint64_t current_generation);
int nb_yfe2_control_target(const uint8_t* route,uint16_t route_length);
int nb_yfe2_probe_route(const uint8_t* route,uint16_t route_length);
size_t nb_yfe2_required_datagram(size_t shard_size);
int nb_yfe2_sender_eligible(uint32_t schema_version,const char* fec_mode,int role,
    int outbound,uint8_t direction,int media,int probe,size_t max_datagram_payload,
    size_t shard_size);
int nb_yfe2_prepare_action(size_t wire_length,size_t allowance,
    size_t max_datagram_payload);
int nb_yfe2_start_action(size_t required_payload,size_t advertised_payload,
    size_t current_path_payload);
int nb_yfe2_control_datagram_encode(uint8_t* out,size_t cap,uint32_t session_id,
    const nb_yfe2_control_t* control);
int nb_yfe2_control_datagram_decode(const uint8_t* wire,size_t length,
    nb_udp_wire_view_t* view,nb_yfe2_control_t* control);

#endif
