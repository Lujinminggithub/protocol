#ifndef NB_UDP_PROBE_ECHO_H
#define NB_UDP_PROBE_ECHO_H

#include "nb_udp.h"

#define NB_PROBE_PORT 9
#define NB_PROBE_SINK_HOST "nb-probe-sink.internal"
#define NB_PROBE_ECHO_HOST "nb-probe-echo.internal"
#define NB_PROBE_SOURCE_HOST "nb-probe-source.internal"

typedef struct {
    int internal_echo_ready;
    int dns_submit;
    int target_socket_open;
    int to_down;
    int tenant_render;
    int fec_source;
    uint8_t type;
    uint32_t sequence;
    const char* route;
    uint16_t route_length;
    const uint8_t* payload;
    uint16_t payload_length;
    uint64_t c2s_packets_delta;
    uint64_t s2c_packets_delta;
    uint64_t c2s_bytes_delta;
    uint64_t s2c_bytes_delta;
} nb_udp_probe_echo_plan_t;

typedef int (*nb_udp_probe_echo_enqueue_fn)(void* opaque,const nb_udp_probe_echo_plan_t* plan);

#define NB_UDP_PROBE_ECHO_REASSEMBLY_ERROR (-2)

int nb_udp_probe_echo_target(const char* host,int port);
int nb_probe_internal_target(const char* host,int port);
int nb_udp_probe_echo_plan(const char* host,int port,const nb_udp_reassembled_t* complete,
    nb_udp_probe_echo_plan_t* plan);
int nb_udp_probe_echo_exit_feed(const char* host,int port,nb_udp_reassembly_t* reassembly,
    const nb_udp_wire_view_t* view,uint64_t now_us,nb_udp_probe_echo_enqueue_fn enqueue,void* opaque);

#endif
