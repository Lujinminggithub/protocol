#include "nb_udp_queue.h"

#include <stdlib.h>
#include <string.h>

#include "nb_udp.h"

typedef struct {
    const uint8_t* wire;
    size_t wire_length;
    size_t record_length;
} queue_record_t;

typedef struct {
    uint32_t group;
    size_t offset;
    size_t length;
} trim_record_t;

typedef struct {
    nb_udp_queue_packet_key_t key;
    size_t bytes;
    int valid_key;
    int selected;
} trim_group_t;

/* 2^17 is the smallest power of two above NB_UDP_QUEUE_MAX_RECORDS (95325). */
#define NB_UDP_QUEUE_TRIM_HASH_CAP 131072u

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

static int sequence_after(uint32_t left,uint32_t right){
    return (int32_t)(left-right)>0;
}

static uint32_t packet_key_hash(const nb_udp_queue_packet_key_t* key){
    uint32_t hash=key->session_id^(key->sequence*0x9e3779b9u)^key->direction;
    hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;hash*=0x846ca68bu;
    return hash^(hash>>16);
}

static nb_udp_queue_drop_window_t* drop_window_for_key(nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key){
    size_t slot=key->direction==NB_UDP_TYPE_C2S?0:1;
    nb_udp_queue_drop_window_t* window=&history->windows[slot];
    if(window->session_id!=key->session_id||window->direction!=key->direction){
        memset(window,0,sizeof(*window));window->direction=key->direction;window->session_id=key->session_id;
    }
    return window;
}

static const nb_udp_queue_drop_window_t* drop_window_find(const nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key){
    size_t slot=key->direction==NB_UDP_TYPE_C2S?0:1;
    const nb_udp_queue_drop_window_t* window=&history->windows[slot];
    return window->direction==key->direction&&window->session_id==key->session_id?window:NULL;
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
    (void)now_us;
    nb_udp_queue_drop_window_t* window=drop_window_for_key(history,key);
    if(window->dropped_sequences==0){window->highest_sequence=key->sequence;window->dropped_sequences=1;return;}
    if(sequence_after(key->sequence,window->highest_sequence)){
        uint32_t advance=key->sequence-window->highest_sequence;
        window->dropped_sequences=advance>=NB_UDP_QUEUE_DROP_WINDOW_BITS?1:
            (window->dropped_sequences<<advance)|1;
        window->highest_sequence=key->sequence;return;
    }
    uint32_t distance=window->highest_sequence-key->sequence;
    if(distance<NB_UDP_QUEUE_DROP_WINDOW_BITS)window->dropped_sequences|=UINT64_C(1)<<distance;
}

int nb_udp_queue_drop_history_contains(const nb_udp_queue_drop_history_t* history,
    const nb_udp_queue_packet_key_t* key,uint64_t now_us,uint64_t max_age_us){
    if(history==NULL||!packet_key_valid(key))return 0;
    (void)now_us;(void)max_age_us;
    const nb_udp_queue_drop_window_t* window=drop_window_find(history,key);
    if(window==NULL||sequence_after(key->sequence,window->highest_sequence))return 0;
    uint32_t distance=window->highest_sequence-key->sequence;
    if(distance>=NB_UDP_QUEUE_DROP_WINDOW_BITS)return 1;
    return (window->dropped_sequences&(UINT64_C(1)<<distance))!=0;
}

static int trim_to_limit_with_hash(uint8_t* queue,size_t* length,size_t need,size_t queue_limit,
    nb_udp_queue_drop_observer_t observer,void* observer_context,size_t* removed_bytes,
    uint64_t* dropped_packets,uint32_t (*hash_key)(const nb_udp_queue_packet_key_t*)){
    if(length==NULL||removed_bytes==NULL||dropped_packets==NULL)return -1;
    *removed_bytes=0;*dropped_packets=0;
    if(*length==0)return need>queue_limit?1:0;
    if(queue==NULL||*length>NB_UDP_QUEUE_MAX_BYTES||need>SIZE_MAX-*length)return -1;
    if(*length+need<=queue_limit)return 0;
    trim_record_t* records=calloc(NB_UDP_QUEUE_MAX_RECORDS,sizeof(*records));
    trim_group_t* groups=calloc(NB_UDP_QUEUE_MAX_RECORDS,sizeof(*groups));
    uint32_t* slots=malloc(NB_UDP_QUEUE_TRIM_HASH_CAP*sizeof(*slots));
    if(records==NULL||groups==NULL||slots==NULL){free(records);free(groups);free(slots);return -1;}
    for(size_t i=0;i<NB_UDP_QUEUE_TRIM_HASH_CAP;i++)slots[i]=UINT32_MAX;
    size_t offset=0,record_count=0,group_count=0;
    while(offset<*length){
        queue_record_t record;if(record_peek(queue+offset,*length-offset,&record)!=0)goto fail;
        if(record_count>=NB_UDP_QUEUE_MAX_RECORDS)goto fail;
        uint8_t direction=0;uint32_t session_id=0,sequence=0;
        uint32_t group=(uint32_t)group_count;
        if(wire_key(&record,&direction,&session_id,&sequence)){
            nb_udp_queue_packet_key_t key={direction,session_id,sequence};
            size_t slot=hash_key(&key)&(NB_UDP_QUEUE_TRIM_HASH_CAP-1u);int found=0;
            for(size_t probe=0;probe<64u;probe++){
                if(slots[slot]==UINT32_MAX){found=1;break;}
                group=slots[slot];
                if(groups[group].valid_key&&groups[group].key.direction==key.direction&&
                    groups[group].key.session_id==key.session_id&&groups[group].key.sequence==key.sequence){found=1;break;}
                slot=(slot+1u)&(NB_UDP_QUEUE_TRIM_HASH_CAP-1u);
            }
            if(!found)goto fail;
            if(slots[slot]==UINT32_MAX){
                if(group_count>=NB_UDP_QUEUE_MAX_RECORDS)goto fail;
                group=(uint32_t)group_count++;groups[group].key=key;groups[group].valid_key=1;slots[slot]=group;
            }
        }else{
            if(group_count>=NB_UDP_QUEUE_MAX_RECORDS)goto fail;
            group=(uint32_t)group_count++;
        }
        records[record_count++] = (trim_record_t){group,offset,record.record_length};
        groups[group].bytes+=record.record_length;offset+=record.record_length;
    }
    size_t removed=0;uint64_t packets=0;
    for(size_t group=0;group<group_count&&*length-removed+need>queue_limit;group++){
        groups[group].selected=1;removed+=groups[group].bytes;packets++;
    }
    size_t write=0;
    for(size_t record=0;record<record_count;record++){
        trim_record_t* current=&records[record];
        if(groups[current->group].selected)continue;
        if(write!=current->offset)memmove(queue+write,queue+current->offset,current->length);
        write+=current->length;
    }
    for(size_t group=0;group<group_count;group++)if(groups[group].selected&&groups[group].valid_key&&observer!=NULL)
        observer(observer_context,&groups[group].key);
    *length=write;*removed_bytes=removed;*dropped_packets=packets;
    free(records);free(groups);free(slots);
    return *length+need>queue_limit?1:0;
fail:
    free(records);free(groups);free(slots);return -1;
}

int nb_udp_queue_trim_to_limit(uint8_t* queue,size_t* length,size_t need,size_t queue_limit,
    nb_udp_queue_drop_observer_t observer,void* observer_context,size_t* removed_bytes,
    uint64_t* dropped_packets){
    return trim_to_limit_with_hash(queue,length,need,queue_limit,observer,observer_context,
        removed_bytes,dropped_packets,packet_key_hash);
}

#ifdef NB_NODE_QUEUE_TEST
static uint32_t forced_collision_hash(const nb_udp_queue_packet_key_t* key){(void)key;return 0;}

int nb_udp_queue_trim_to_limit_forced_hash_test(uint8_t* queue,size_t* length,size_t need,
    size_t queue_limit,size_t* removed_bytes,uint64_t* dropped_packets){
    return trim_to_limit_with_hash(queue,length,need,queue_limit,NULL,NULL,removed_bytes,
        dropped_packets,forced_collision_hash);
}
#endif

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
        queue_record_t record;if(record_peek(queue+read,*length-read,&record)!=0)return -1;
        if(!wire_key_matches(&record,direction,session_id,sequence)){
            if(write!=read)memmove(queue+write,queue+read,record.record_length);
            write+=record.record_length;
        }
        read+=record.record_length;
    }
    *length=write;
    *removed_bytes=removed;*dropped_packets=1;return 0;
}
