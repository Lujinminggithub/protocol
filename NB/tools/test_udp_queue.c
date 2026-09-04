#include "nb_udp.h"
#include "nb_udp_queue.h"
#include "nb_live.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
int nb_node_udp_queue_trim_for_test(uint8_t* queue,size_t* length,size_t need,size_t queue_limit,
    uint64_t instance_queue_bytes,uint64_t instance_queue_limit,nb_udp_queue_drop_history_t* history,
    uint64_t now_us,size_t* released,uint64_t* dropped);
int nb_node_udp_queue_admit_for_test(uint8_t* queue,size_t* length,size_t need,size_t queue_limit,
    nb_udp_queue_drop_history_t* history,const nb_udp_queue_packet_key_t* incoming_key,
    uint64_t now_us,uint64_t* pressure_dropped);
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

#ifdef NB_NODE_QUEUE_TEST
static size_t append_raw_record(uint8_t* queue,size_t offset,uint8_t value,uint64_t queued_at){
    put16(queue+offset,1);put64(queue+offset+2,queued_at);queue[offset+10]=value;
    return offset+11;
}
#endif

int main(void){
    uint8_t queue[256]={0};size_t length=0;
#ifdef NB_NODE_QUEUE_TEST
    size_t down_initial_length=0,down_initial_released=0;uint64_t down_initial_dropped=0;
    nb_udp_queue_drop_history_t down_initial_history={0};
    CHECK(nb_node_udp_queue_trim_for_test(NULL,&down_initial_length,36,108,0,1024,&down_initial_history,1,
        &down_initial_released,&down_initial_dropped)==0);
    size_t up_initial_length=0,up_initial_released=0;uint64_t up_initial_dropped=0;
    nb_udp_queue_drop_history_t up_initial_history={0};
    CHECK(nb_node_udp_queue_trim_for_test(NULL,&up_initial_length,36,108,0,1024,&up_initial_history,2,
        &up_initial_released,&up_initial_dropped)==0);
    size_t pending_initial_length=0,pending_initial_released=0;uint64_t pending_initial_dropped=0;
    nb_udp_queue_drop_history_t pending_initial_history={0};
    CHECK(nb_node_udp_queue_trim_for_test(NULL,&pending_initial_length,36,108,0,1024,
        &pending_initial_history,3,&pending_initial_released,&pending_initial_dropped)==0);
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
    CHECK(nb_udp_queue_drop_history_contains(&history,&dropped_key,12001,5000));
    for(uint32_t sequence=12;sequence<=44;sequence++){
        nb_udp_queue_packet_key_t newer={NB_UDP_TYPE_C2S,7,sequence};
        nb_udp_queue_drop_history_note(&history,&newer,7000+sequence);
    }
    CHECK(nb_udp_queue_drop_history_contains(&history,&dropped_key,20000,5000));
    nb_udp_queue_drop_history_t wrapped={0};
    nb_udp_queue_packet_key_t wrapped_key={NB_UDP_TYPE_S2C,9,UINT32_MAX-1};
    nb_udp_queue_drop_history_note(&wrapped,&wrapped_key,1);
    nb_udp_queue_packet_key_t wrapped_newer={NB_UDP_TYPE_S2C,9,0};
    nb_udp_queue_drop_history_note(&wrapped,&wrapped_newer,2);
    CHECK(nb_udp_queue_drop_history_contains(&wrapped,&wrapped_key,3,1));
#ifdef NB_NODE_QUEUE_TEST
    uint8_t hard_limit[256]={0};size_t hard_limit_length=0;uint64_t hard_dropped=0;
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,21,0,1000);
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,22,0,1001);
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,21,1,1002);
    hard_limit_length=append_fragment(hard_limit,hard_limit_length,22,1,1003);
    nb_udp_queue_drop_history_t hard_history={0};
    int hard_rc=nb_node_udp_queue_make_hard_room_for_test(hard_limit,&hard_limit_length,36,108,
        &hard_history,7000,&hard_dropped);
    CHECK(hard_rc==0);
    CHECK(hard_dropped==1&&hard_limit_length==72);
    CHECK(hard_limit[22]==0&&hard_limit[23]==0&&hard_limit[24]==0&&hard_limit[25]==22);
    nb_udp_queue_packet_key_t late_key={NB_UDP_TYPE_C2S,7,21};
    CHECK(nb_udp_queue_drop_history_contains(&hard_history,&late_key,7001,5000));
    CHECK(nb_node_udp_queue_should_reject_for_test(&hard_history,&late_key,7001));
    uint8_t incoming_trim[64]={0};size_t incoming_length=0;uint64_t incoming_pressure=0;
    incoming_length=append_fragment(incoming_trim,incoming_length,23,0,1000);
    nb_udp_queue_packet_key_t incoming_key={NB_UDP_TYPE_C2S,7,23};
    nb_udp_queue_drop_history_t incoming_history={0};
    CHECK(nb_node_udp_queue_admit_for_test(incoming_trim,&incoming_length,36,35,&incoming_history,
        &incoming_key,7500,&incoming_pressure)==1);
    CHECK(incoming_length==0&&incoming_pressure==1);
    CHECK(nb_node_udp_queue_should_reject_for_test(&incoming_history,&incoming_key,7501));
    uint8_t multi_trim[256]={0};size_t multi_trim_length=0;uint64_t multi_dropped=0;
    multi_trim_length=append_fragment(multi_trim,multi_trim_length,31,0,1000);
    multi_trim_length=append_fragment(multi_trim,multi_trim_length,32,0,1001);
    multi_trim_length=append_fragment(multi_trim,multi_trim_length,33,0,1002);
    multi_trim_length=append_fragment(multi_trim,multi_trim_length,31,1,1003);
    multi_trim_length=append_fragment(multi_trim,multi_trim_length,32,1,1004);
    multi_trim_length=append_fragment(multi_trim,multi_trim_length,33,1,1005);
    nb_udp_queue_drop_history_t multi_history={0};
    CHECK(nb_node_udp_queue_make_hard_room_for_test(multi_trim,&multi_trim_length,36,108,
        &multi_history,8000,&multi_dropped)==0);
    CHECK(multi_dropped==2&&multi_trim_length==72);
    CHECK(multi_trim[22]==0&&multi_trim[23]==0&&multi_trim[24]==0&&multi_trim[25]==33);
    CHECK(multi_trim[58]==0&&multi_trim[59]==0&&multi_trim[60]==0&&multi_trim[61]==33);
    uint8_t budget_trim[256]={0};size_t budget_trim_length=0,budget_released=0;uint64_t budget_dropped=0;
    for(uint32_t sequence=41;sequence<=44;sequence++)
        budget_trim_length=append_fragment(budget_trim,budget_trim_length,sequence,0,1000+sequence);
    nb_udp_queue_drop_history_t budget_history={0};
    CHECK(nb_node_udp_queue_trim_for_test(budget_trim,&budget_trim_length,36,1024*1024,
        budget_trim_length+200,300,&budget_history,9000,&budget_released,&budget_dropped)==0);
    CHECK(budget_trim_length==36&&budget_released==108&&budget_dropped==3);
    uint8_t pending_trim[64]={0};size_t pending_length=0,pending_released=0;uint64_t pending_dropped=0;
    pending_length=append_raw_record(pending_trim,pending_length,1,1000);
    pending_length=append_raw_record(pending_trim,pending_length,2,1001);
    pending_length=append_raw_record(pending_trim,pending_length,3,1002);
    nb_udp_queue_drop_history_t pending_history={0};
    CHECK(nb_node_udp_queue_trim_for_test(pending_trim,&pending_length,11,33,pending_length,1024,
        &pending_history,9500,&pending_released,&pending_dropped)==0);
    CHECK(pending_length==22&&pending_released==11&&pending_dropped==1&&pending_trim[10]==2);
    uint8_t* shrink_trim=malloc(1024*1024);CHECK(shrink_trim!=NULL);
    size_t shrink_length=0,shrink_released=0;uint64_t shrink_dropped=0;uint32_t shrink_sequence=1000;
    while(shrink_length+36<=1024*1024)
        shrink_length=append_fragment(shrink_trim,shrink_length,shrink_sequence++,0,1000);
    size_t shrink_initial=shrink_length;nb_udp_queue_drop_history_t shrink_history={0};
    CHECK(nb_node_udp_queue_trim_for_test(shrink_trim,&shrink_length,36,256*1024,
        shrink_initial,1024*1024,&shrink_history,10000,&shrink_released,&shrink_dropped)==0);
    CHECK(shrink_length+36<=256*1024&&shrink_released==shrink_initial-shrink_length);
    CHECK(shrink_dropped==shrink_released/36);free(shrink_trim);
    uint8_t low_bits[4096]={0};size_t low_bits_length=0,low_bits_removed=0;uint64_t low_bits_dropped=0;
    for(uint32_t index=0;index<65;index++)
        low_bits_length=append_fragment(low_bits,low_bits_length,300+index*131072u,0,1000);
    CHECK(nb_node_udp_queue_trim_for_test(low_bits,&low_bits_length,36,36,low_bits_length,1024*1024,
        &down_initial_history,11000,&low_bits_removed,&low_bits_dropped)==0);
    CHECK(low_bits_length==0&&low_bits_removed==65*36&&low_bits_dropped==65);
    uint8_t forced_collision[4096]={0};size_t forced_length=0,forced_removed=99;uint64_t forced_dropped=99;
    for(uint32_t sequence=500;sequence<565;sequence++)
        forced_length=append_fragment(forced_collision,forced_length,sequence,0,1000);
    size_t forced_original=forced_length;
    CHECK(nb_udp_queue_trim_to_limit_forced_hash_test(forced_collision,&forced_length,36,0,
        &forced_removed,&forced_dropped)==-1);
    CHECK(forced_length==forced_original&&forced_removed==0&&forced_dropped==0);
#endif
    CHECK(nb_udp_queue_oldest_age_us(queue,9,6000)==0);
    put64(queue+2,7000);
    CHECK(nb_udp_queue_oldest_age_us(queue,length,6000)==0);
    puts("nb_udp_queue_test: ok");return 0;
}
