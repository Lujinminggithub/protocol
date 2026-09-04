#include "nb_udp_probe_echo.h"
#include "nb_udp_lifecycle.h"

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

typedef struct {
    uint8_t udp_up_tx[4096];
    size_t udp_up_tx_length;
    unsigned udp_up_tx_calls;
    unsigned tenant_hook_calls;
    unsigned fec_hook_calls;
    nb_udp_probe_echo_plan_t plan;
} echo_queue_t;

static int queue_echo_up(void* opaque,const nb_udp_probe_echo_plan_t* plan){
    echo_queue_t* queue=opaque;
    if(queue==NULL||plan==NULL||plan->type!=NB_UDP_TYPE_S2C)return -1;
    queue->plan=*plan;queue->udp_up_tx_calls++;
    queue->tenant_hook_calls+=(unsigned)plan->tenant_render;
    queue->fec_hook_calls+=(unsigned)plan->fec_source;
    uint16_t fragments=nb_udp_fragment_count(plan->payload_length);
    for(uint16_t index=0;index<fragments;index++){
        size_t offset=(size_t)index*NB_UDP_FRAGMENT_PAYLOAD;
        size_t payload_length=plan->payload_length-offset;
        if(payload_length>NB_UDP_FRAGMENT_PAYLOAD)payload_length=NB_UDP_FRAGMENT_PAYLOAD;
        uint8_t wire[1400];
        int length=nb_udp_wire_encode(wire,sizeof(wire),plan->type,71,plan->sequence,index,fragments,
            plan->payload_length,plan->route,plan->route_length,plan->payload+offset,payload_length);
        if(length<0||queue->udp_up_tx_length+2u+(size_t)length>sizeof(queue->udp_up_tx))return -1;
        queue->udp_up_tx[queue->udp_up_tx_length++]=(uint8_t)(length>>8);
        queue->udp_up_tx[queue->udp_up_tx_length++]=(uint8_t)length;
        memcpy(queue->udp_up_tx+queue->udp_up_tx_length,wire,(size_t)length);
        queue->udp_up_tx_length+=(size_t)length;
    }
    return 0;
}

static void test_multifragment_exit_echo(void){
    uint8_t payload[2501];for(size_t index=0;index<sizeof(payload);index++)payload[index]=(uint8_t)(index*17u);
    const char route[]="N=tenant-a;T:nb-probe-echo.internal:9";
    nb_udp_reassembly_t exit_reassembly,entry_reassembly;nb_udp_reassembly_init(&exit_reassembly);
    nb_udp_reassembly_init(&entry_reassembly);echo_queue_t queue={0};
    uint16_t fragments=nb_udp_fragment_count(sizeof(payload));assert(fragments==3);
    for(uint16_t index=0;index<fragments;index++){
        size_t offset=(size_t)index*NB_UDP_FRAGMENT_PAYLOAD;
        size_t payload_length=sizeof(payload)-offset;if(payload_length>NB_UDP_FRAGMENT_PAYLOAD)payload_length=NB_UDP_FRAGMENT_PAYLOAD;
        uint8_t wire[1400];
        int length=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,71,0, index,fragments,
            sizeof(payload),route,strlen(route),payload+offset,payload_length);
        assert(length>0);nb_udp_wire_view_t view;assert(nb_udp_wire_decode(wire,(size_t)length,&view)==0);
        int rc=nb_udp_probe_echo_exit_feed(NB_PROBE_ECHO_HOST,NB_PROBE_PORT,&exit_reassembly,
            &view,1000+index,queue_echo_up,&queue);
        assert(rc==(index+1==fragments?1:0));
    }
    assert(queue.udp_up_tx_calls==1&&queue.udp_up_tx_length>0);
    assert(queue.tenant_hook_calls==0&&queue.fec_hook_calls==0);
    assert(queue.plan.to_down==0&&queue.plan.tenant_render==0&&queue.plan.fec_source==0);
    assert(queue.plan.dns_submit==0&&queue.plan.target_socket_open==0);
    assert(queue.plan.sequence==0&&queue.plan.route_length==strlen(route));
    assert(memcmp(queue.plan.route,route,strlen(route))==0);
    assert(queue.plan.payload_length==sizeof(payload)&&memcmp(queue.plan.payload,payload,sizeof(payload))==0);
    assert(queue.plan.c2s_packets_delta==1&&queue.plan.s2c_packets_delta==1);
    assert(queue.plan.c2s_bytes_delta==sizeof(payload)&&queue.plan.s2c_bytes_delta==sizeof(payload));
    size_t offset=0;nb_udp_reassembled_t complete={0};int complete_count=0;
    while(offset<queue.udp_up_tx_length){
        size_t length=((size_t)queue.udp_up_tx[offset]<<8)|queue.udp_up_tx[offset+1];offset+=2;
        nb_udp_wire_view_t view;assert(nb_udp_wire_decode(queue.udp_up_tx+offset,length,&view)==0);
        assert(view.type==NB_UDP_TYPE_S2C&&view.session_id==71&&view.sequence==0);
        int rc=nb_udp_reassembly_feed(&entry_reassembly,&view,2000+offset,&complete);
        complete_count+=rc;offset+=length;
    }
    assert(complete_count==1&&complete.sequence==0&&complete.payload_length==sizeof(payload));
    assert(strcmp(complete.route,route)==0&&memcmp(complete.payload,payload,sizeof(payload))==0);
    nb_udp_reassembly_dispose(&entry_reassembly);nb_udp_reassembly_dispose(&exit_reassembly);
}

static void test_close_and_idle_helpers(void){
    nb_udp_lifecycle_t lifecycle;nb_udp_lifecycle_init(&lifecycle);
    assert(nb_udp_lifecycle_schedule(&lifecycle,17,NB_UDP_TYPE_CLOSE,71,
        "T:nb-probe-echo.internal:9",100)==0);
    assert(nb_udp_lifecycle_ack(&lifecycle,17,71)==1&&nb_udp_lifecycle_count(&lifecycle)==0);
    assert(!nb_udp_control_grace_expired(100,100,100+NB_UDP_CONTROL_GRACE_US-1,
        NB_UDP_CONTROL_GRACE_US));
    assert(nb_udp_control_grace_expired(100,100,100+NB_UDP_CONTROL_GRACE_US,
        NB_UDP_CONTROL_GRACE_US));
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
    test_multifragment_exit_echo();
    test_close_and_idle_helpers();
    puts("nb_udp_probe_echo_test: ok");
    return 0;
}
