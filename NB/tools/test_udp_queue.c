#include "nb_udp.h"
#include "nb_udp_queue.h"
#include "nb_live.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

#ifdef NB_NODE_QUEUE_TEST
size_t nb_node_udp_queue_limit_for_test(nb_flow_class_t flow_class,int to_down,
    int egress_active,uint64_t egress_rate_bps,uint64_t egress_reorder_us,
    int ingress_active,uint64_t ingress_rate_bps,uint64_t ingress_reorder_us);
int nb_node_udp_queue_make_hard_room_for_test(uint8_t* queue,size_t* length,size_t need,
    size_t queue_limit,nb_udp_queue_drop_history_t* history,uint64_t now_us,uint64_t* dropped);
int nb_node_udp_queue_should_reject_for_test(const nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us);
#endif

static void put16(uint8_t* p,uint16_t value){p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value;}
static void put32(uint8_t* p,uint32_t value){p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;}
static void put64(uint8_t* p,uint64_t value){
    for(size_t i=0;i<8;i++)p[i]=(uint8_t)(value>>(56u-8u*i));
}

static size_t append_fragment(uint8_t* queue,size_t offset,uint32_t sequence,uint16_t fragment,uint64_t queued_at){
    uint8_t wire[40]={0};put32(wire,NB_UDP_MAGIC);wire[4]=NB_UDP_VERSION;wire[5]=NB_UDP_TYPE_C2S;
    put16(wire+6,24);put32(wire+8,7);put32(wire+12,sequence);put16(wire+16,fragment);
    put16(wire+18,2);put16(wire+20,2);put16(wire+22,1);wire[24]='R';wire[25]=(uint8_t)fragment;
    put16(queue+offset,26);put64(queue+offset+2,queued_at);memcpy(queue+offset+10,wire,26);
    return offset+36;
}

int main(void){
    uint8_t queue[256]={0};size_t length=0;
#ifdef NB_NODE_QUEUE_TEST
    CHECK(nb_node_udp_queue_limit_for_test(NB_FLOW_CLASS_MEDIA,1,
        1,5000000,462000,1,1000000,200000)==589824);
    CHECK(nb_node_udp_queue_limit_for_test(NB_FLOW_CLASS_MEDIA,0,
        1,1000000,200000,1,5000000,462000)==589824);
    CHECK(nb_node_udp_queue_limit_for_test(NB_FLOW_CLASS_MEDIA,1,
        0,5000000,462000,1,5000000,462000)==262144);
#endif
    CHECK(nb_udp_queue_oldest_age_us(queue,length,6000)==0);
    length=append_fragment(queue,length,1,0,1000);length=append_fragment(queue,length,1,1,1000);
    length=append_fragment(queue,length,2,0,2000);length=append_fragment(queue,length,2,1,2000);
    CHECK(nb_udp_queue_oldest_age_us(queue,length,6000)==5000);
    size_t removed=0;uint64_t packets=0;
    CHECK(nb_udp_queue_drop_oldest_packet(queue,&length,&removed,&packets)==0);
    CHECK(removed==72&&packets==1&&length==72);
    CHECK(queue[22]==0&&queue[23]==0&&queue[24]==0&&queue[25]==2);
    CHECK(nb_udp_queue_oldest_age_us(queue,length,6000)==4000);
    uint8_t interleaved[256]={0};size_t interleaved_length=0;
    interleaved_length=append_fragment(interleaved,interleaved_length,11,0,1000);
    interleaved_length=append_fragment(interleaved,interleaved_length,12,0,1001);
    interleaved_length=append_fragment(interleaved,interleaved_length,11,1,1002);
    interleaved_length=append_fragment(interleaved,interleaved_length,12,1,1003);
    nb_udp_queue_packet_key_t dropped_key={0};
    CHECK(nb_udp_queue_oldest_packet_key(interleaved,interleaved_length,&dropped_key)==0);
    CHECK(dropped_key.direction==NB_UDP_TYPE_C2S&&dropped_key.session_id==7&&dropped_key.sequence==11);
    CHECK(nb_udp_queue_drop_oldest_packet(interleaved,&interleaved_length,&removed,&packets)==0);
    CHECK(removed==72&&packets==1&&interleaved_length==72);
    CHECK(interleaved[22]==0&&interleaved[23]==0&&interleaved[24]==0&&interleaved[25]==12);
    CHECK(interleaved[58]==0&&interleaved[59]==0&&interleaved[60]==0&&interleaved[61]==12);
    nb_udp_queue_drop_history_t history={0};
    nb_udp_queue_drop_history_note(&history,&dropped_key,6000);
    CHECK(nb_udp_queue_drop_history_contains(&history,&dropped_key,6001,5000));
    dropped_key.sequence=12;
    CHECK(!nb_udp_queue_drop_history_contains(&history,&dropped_key,6001,5000));
    dropped_key.sequence=11;
    CHECK(!nb_udp_queue_drop_history_contains(&history,&dropped_key,12001,5000));
#ifdef NB_NODE_QUEUE_TEST
    uint8_t hard_limit[256]={0};size_t hard_limit_length=0;uint64_t hard_dropped=0;
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,21,0,1000);
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,22,0,1001);
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,21,1,1002);
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,22,1,1003);
    nb_udp_queue_drop_history_t hard_history={0};
    CHECK(nb_node_udp_queue_make_hard_room_for_test(hard_limit,&hard_limit_length,36,108,
        &hard_history,7000,&hard_dropped)==0);
    CHECK(hard_dropped==1&&hard_limit_length==72);
    CHECK(hard_limit[22]==0&&hard_limit[23]==0&&hard_limit[24]==0&&hard_limit[25]==22);
    nb_udp_queue_packet_key_t late_key={NB_UDP_TYPE_C2S,7,21};
    CHECK(nb_udp_queue_drop_history_contains(&hard_history,&late_key,7001,5000));
    CHECK(nb_node_udp_queue_should_reject_for_test(&hard_history,&late_key,7001));
#endif
    CHECK(nb_udp_queue_oldest_age_us(queue,9,6000)==0);
    put64(queue+2,7000);
    CHECK(nb_udp_queue_oldest_age_us(queue,length,6000)==0);
    puts("nb_udp_queue_test: ok");return 0;
}
