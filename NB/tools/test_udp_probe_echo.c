#include "nb_udp_probe_echo.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static nb_udp_reassembled_t complete_packet(uint32_t sequence,const char* route,
    const uint8_t* payload,size_t payload_length){
    nb_udp_reassembled_t complete={0};
    complete.sequence=sequence;complete.route=route;complete.route_length=(uint16_t)strlen(route);
    complete.payload=payload;complete.payload_length=(uint16_t)payload_length;
    return complete;
}

int main(void){
    const uint8_t payload[]={0x00,0x7f,0x80,0xff};
    const char route[]="T:nb-probe-echo.internal:9";
    nb_udp_reassembled_t complete=complete_packet(41,route,payload,sizeof(payload));
    nb_udp_probe_echo_plan_t plan={0};

    assert(nb_udp_probe_echo_plan("NB-PROBE-ECHO.INTERNAL",9,&complete,&plan)==1);
    assert(plan.internal_echo_ready==1);
    assert(plan.dns_submit==0&&plan.target_socket_open==0);
    assert(plan.type==NB_UDP_TYPE_S2C&&plan.sequence==41);
    assert(plan.route==complete.route&&plan.route_length==complete.route_length);
    assert(plan.payload==complete.payload&&plan.payload_length==complete.payload_length);
    assert(plan.c2s_packets_delta==1&&plan.s2c_packets_delta==1);
    assert(plan.c2s_bytes_delta==sizeof(payload)&&plan.s2c_bytes_delta==sizeof(payload));
    assert(nb_probe_internal_target(NB_PROBE_SINK_HOST,9)==1);
    assert(nb_probe_internal_target(NB_PROBE_ECHO_HOST,9)==1);
    assert(nb_probe_internal_target(NB_PROBE_SOURCE_HOST,9)==1);
    assert(nb_probe_internal_target(NB_PROBE_ECHO_HOST,10)==0);

    complete.sequence=0;
    assert(nb_udp_probe_echo_plan("nb-probe-echo.internal",9,&complete,&plan)==1);
    assert(plan.sequence==0);
    complete.sequence=41;

    memset(&plan,0xa5,sizeof(plan));
    assert(nb_udp_probe_echo_plan("nb-probe-echo.internal",10,&complete,&plan)==0);
    assert(plan.internal_echo_ready==0&&plan.dns_submit==0&&plan.target_socket_open==0);
    assert(plan.c2s_packets_delta==0&&plan.s2c_packets_delta==0);
    assert(nb_udp_probe_echo_plan("other.nb-probe-echo.internal",9,&complete,&plan)==0);
    assert(nb_udp_probe_echo_plan("nb-probe-echo.internal.invalid",9,&complete,&plan)==0);
    assert(nb_udp_probe_echo_plan("nb-probe-sink.internal",9,&complete,&plan)==0);
    assert(nb_udp_probe_echo_plan("nb-probe-echo.internal",9,NULL,&plan)==-1);
    assert(nb_udp_probe_echo_plan(NULL,9,&complete,&plan)==-1);
    puts("nb_udp_probe_echo_test: ok");
    return 0;
}
