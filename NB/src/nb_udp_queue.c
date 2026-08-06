#include "nb_udp_queue.h"

#include <string.h>

#include "nb_udp.h"

typedef struct {
    const uint8_t* wire;
    size_t wire_length;
    size_t record_length;
} queue_record_t;

static uint16_t get16(const uint8_t* p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t* p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}

static int record_peek(const uint8_t* queue,size_t length,queue_record_t* record){
    if(queue==NULL||record==NULL||length<NB_UDP_QUEUE_RECORD_HEADER)return -1;
    size_t wire_length=get16(queue);size_t record_length=NB_UDP_QUEUE_RECORD_HEADER+wire_length;
    if(wire_length==0||record_length>length)return -1;
    record->wire=queue+NB_UDP_QUEUE_RECORD_HEADER;record->wire_length=wire_length;
    record->record_length=record_length;return 0;
}

static int wire_key(const queue_record_t* record,uint8_t* direction,
    uint32_t* session_id,uint32_t* sequence){
    const uint8_t* wire=record->wire;
    if(record->wire_length<24||get32(wire)!=NB_UDP_MAGIC||wire[4]!=NB_UDP_VERSION||
        (wire[5]!=NB_UDP_TYPE_C2S&&wire[5]!=NB_UDP_TYPE_S2C)||get16(wire+6)!=24||
        get32(wire+8)==0)return 0;
    *direction=wire[5];*session_id=get32(wire+8);*sequence=get32(wire+12);return 1;
}

int nb_udp_queue_drop_oldest_packet(uint8_t* queue,size_t* length,
    size_t* removed_bytes,uint64_t* dropped_packets){
    if(queue==NULL||length==NULL||removed_bytes==NULL||dropped_packets==NULL||*length==0)return -1;
    queue_record_t first;if(record_peek(queue,*length,&first)!=0)return -1;
    uint8_t direction=0;uint32_t session_id=0,sequence=0;
    int grouped=wire_key(&first,&direction,&session_id,&sequence);size_t removed=first.record_length;
    while(grouped&&removed<*length){
        queue_record_t next;if(record_peek(queue+removed,*length-removed,&next)!=0)return -1;
        uint8_t next_direction=0;uint32_t next_session=0,next_sequence=0;
        if(!wire_key(&next,&next_direction,&next_session,&next_sequence)||
            next_direction!=direction||next_session!=session_id||next_sequence!=sequence)break;
        removed+=next.record_length;
    }
    memmove(queue,queue+removed,*length-removed);*length-=removed;
    *removed_bytes=removed;*dropped_packets=1;return 0;
}
