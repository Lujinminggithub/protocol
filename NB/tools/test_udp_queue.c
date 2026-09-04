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
    CHECK(nb_udp_queue_oldest_age_us(queue,9,6000)==0);
    put64(queue+2,7000);
    CHECK(nb_udp_queue_oldest_age_us(queue,length,6000)==0);
    puts("nb_udp_queue_test: ok");return 0;
}
