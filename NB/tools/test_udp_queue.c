#include "nb_udp.h"
#include "nb_udp_queue.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

static void put16(uint8_t* p,uint16_t value){p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value;}
static void put32(uint8_t* p,uint32_t value){p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;}

static size_t append_fragment(uint8_t* queue,size_t offset,uint32_t sequence,uint16_t fragment){
    uint8_t wire[40]={0};put32(wire,NB_UDP_MAGIC);wire[4]=NB_UDP_VERSION;wire[5]=NB_UDP_TYPE_C2S;
    put16(wire+6,24);put32(wire+8,7);put32(wire+12,sequence);put16(wire+16,fragment);
    put16(wire+18,2);put16(wire+20,2);put16(wire+22,1);wire[24]='R';wire[25]=(uint8_t)fragment;
    put16(queue+offset,26);memset(queue+offset+2,0,8);memcpy(queue+offset+10,wire,26);
    return offset+36;
}

int main(void){
    uint8_t queue[256]={0};size_t length=0;
    length=append_fragment(queue,length,1,0);length=append_fragment(queue,length,1,1);
    length=append_fragment(queue,length,2,0);length=append_fragment(queue,length,2,1);
    size_t removed=0;uint64_t packets=0;
    CHECK(nb_udp_queue_drop_oldest_packet(queue,&length,&removed,&packets)==0);
    CHECK(removed==72&&packets==1&&length==72);
    CHECK(queue[22]==0&&queue[23]==0&&queue[24]==0&&queue[25]==2);
    puts("nb_udp_queue_test: ok");return 0;
}
