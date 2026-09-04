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
static uint64_t get64(const uint8_t* p){
    uint64_t value=0;
    for(size_t i=0;i<8;i++)value=(value<<8)|p[i];
    return value;
}

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

static int wire_key_matches(const queue_record_t* record,uint8_t direction,
    uint32_t session_id,uint32_t sequence){
    uint8_t candidate_direction=0;uint32_t candidate_session=0,candidate_sequence=0;
    return wire_key(record,&candidate_direction,&candidate_session,&candidate_sequence)&&
        candidate_direction==direction&&candidate_session==session_id&&candidate_sequence==sequence;
}

static int packet_key_valid(const nb_udp_queue_packet_key_t* key){
    return key!=NULL&&(key->direction==NB_UDP_TYPE_C2S||key->direction==NB_UDP_TYPE_S2C)&&
        key->session_id!=0;
}

static int packet_key_equal(const nb_udp_queue_packet_key_t* left,
    const nb_udp_queue_packet_key_t* right){
    return left->direction==right->direction&&left->session_id==right->session_id&&
        left->sequence==right->sequence;
}

uint64_t nb_udp_queue_oldest_age_us(const uint8_t* queue,size_t length,uint64_t now_us){
    queue_record_t first;
    if(record_peek(queue,length,&first)!=0)return 0;
    uint64_t queued_at=get64(queue+2);
    if(queued_at>now_us)return 0;
    return now_us-queued_at;
}

int nb_udp_queue_oldest_packet_key(const uint8_t* queue,size_t length,
    nb_udp_queue_packet_key_t* key){
    queue_record_t first;
    if(key==NULL||record_peek(queue,length,&first)!=0)return -1;
    return wire_key(&first,&key->direction,&key->session_id,&key->sequence)?0:-1;
}

void nb_udp_queue_drop_history_note(nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us){
    if(history==NULL||!packet_key_valid(key))return;
    for(size_t i=0;i<NB_UDP_QUEUE_DROPPED_HISTORY;i++){
        if(packet_key_valid(&history->keys[i])&&packet_key_equal(&history->keys[i],key)){
            history->dropped_at[i]=now_us;return;
        }
    }
    size_t slot=history->next%NB_UDP_QUEUE_DROPPED_HISTORY;
    history->keys[slot]=*key;history->dropped_at[slot]=now_us;
    history->next=(slot+1)%NB_UDP_QUEUE_DROPPED_HISTORY;
}

int nb_udp_queue_drop_history_contains(const nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us,uint64_t max_age_us){
    if(history==NULL||!packet_key_valid(key))return 0;
    for(size_t i=0;i<NB_UDP_QUEUE_DROPPED_HISTORY;i++){
        uint64_t dropped_at=history->dropped_at[i];
        if(packet_key_valid(&history->keys[i])&&packet_key_equal(&history->keys[i],key)&&
            now_us>=dropped_at&&now_us-dropped_at<=max_age_us)return 1;
    }
    return 0;
}

int nb_udp_queue_drop_oldest_packet(uint8_t* queue,size_t* length,
    size_t* removed_bytes,uint64_t* dropped_packets){
    if(queue==NULL||length==NULL||removed_bytes==NULL||dropped_packets==NULL||*length==0)return -1;
    queue_record_t first;if(record_peek(queue,*length,&first)!=0)return -1;
    uint8_t direction=0;uint32_t session_id=0,sequence=0;
    int grouped=wire_key(&first,&direction,&session_id,&sequence);
    if(!grouped){
        size_t removed=first.record_length;
        memmove(queue,queue+removed,*length-removed);*length-=removed;
        *removed_bytes=removed;*dropped_packets=1;return 0;
    }
    size_t read=0,removed=0;
    while(read<*length){
        queue_record_t record;if(record_peek(queue+read,*length-read,&record)!=0)return -1;
        if(wire_key_matches(&record,direction,session_id,sequence))removed+=record.record_length;
        read+=record.record_length;
    }
    size_t write=0;read=0;
    while(read<*length){
        queue_record_t record;/* The validation pass above makes this read safe. */
        (void)record_peek(queue+read,*length-read,&record);
        if(!wire_key_matches(&record,direction,session_id,sequence)){
            if(write!=read)memmove(queue+write,queue+read,record.record_length);
            write+=record.record_length;
        }
        read+=record.record_length;
    }
    *length=write;
    *removed_bytes=removed;*dropped_packets=1;return 0;
}
