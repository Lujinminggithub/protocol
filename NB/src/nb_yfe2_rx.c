#define _POSIX_C_SOURCE 200809L
#include "nb_yfe2_rx.h"

#include "nb_fec_rs.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define NB_YFE2_RX_CACHE 512u

typedef struct {int valid,delivered;uint64_t generation,seen_at;uint32_t session_id;nb_yfe2_desc_t desc;uint8_t payload[NB_UDP_FRAGMENT_PAYLOAD];} source_item_t;
typedef struct {int valid;uint64_t generation,first_seen;uint32_t session_id,block_id;uint8_t flags,r,count;uint16_t shard_size;nb_yfe2_desc_t desc[16];uint8_t repair_present[3];uint8_t repair[3][NB_UDP_FRAGMENT_PAYLOAD];} block_t;
struct nb_yfe2_rx {source_item_t cache[NB_YFE2_RX_CACHE];size_t cache_next;block_t blocks[NB_YFE2_RX_FLOW_BLOCKS];nb_yfe2_rx_metrics_t metrics;};
static _Atomic size_t global_allocated_bytes;
static _Atomic size_t global_active_blocks;

static uint64_t monotonic_ns(void){
    struct timespec value;
    return clock_gettime(CLOCK_MONOTONIC,&value)==0?
        (uint64_t)value.tv_sec*1000000000ULL+(uint64_t)value.tv_nsec:0;
}
static void memory_high_update(nb_yfe2_rx_t* state){
    size_t used=0;
    for(size_t i=0;i<NB_YFE2_RX_CACHE;i++)if(state->cache[i].valid)used+=sizeof(state->cache[i]);
    for(size_t i=0;i<NB_YFE2_RX_FLOW_BLOCKS;i++)if(state->blocks[i].valid)used+=sizeof(state->blocks[i]);
    if(used>state->metrics.memory_high)state->metrics.memory_high=used;
}

static int desc_equal(const nb_yfe2_desc_t* a,const nb_yfe2_desc_t* b){return a->sequence==b->sequence&&
    a->fragment_index==b->fragment_index&&a->fragment_count==b->fragment_count&&
    a->total_length==b->total_length&&a->payload_length==b->payload_length;}
static source_item_t* cache_find(nb_yfe2_rx_t* state,uint64_t generation,uint32_t sid,const nb_yfe2_desc_t* desc){
    for(size_t i=0;i<NB_YFE2_RX_CACHE;i++){source_item_t* item=&state->cache[i];
        if(item->valid&&item->generation==generation&&item->session_id==sid&&desc_equal(&item->desc,desc))return item;}
    return NULL;
}
static source_item_t* cache_store(nb_yfe2_rx_t* state,uint64_t generation,uint32_t sid,
    const nb_yfe2_desc_t* desc,const uint8_t* payload,uint64_t now,int delivered){
    source_item_t* item=cache_find(state,generation,sid,desc);
    if(item==NULL)item=&state->cache[state->cache_next++%NB_YFE2_RX_CACHE];
    memset(item,0,sizeof(*item));item->valid=1;item->delivered=delivered;item->generation=generation;
    item->session_id=sid;item->desc=*desc;item->seen_at=now;memcpy(item->payload,payload,desc->payload_length);
    memory_high_update(state);return item;
}
nb_yfe2_rx_t* nb_yfe2_rx_create(void){
    size_t current=atomic_load(&global_allocated_bytes);
    do {
        if(current>NB_YFE2_RX_GLOBAL_BYTES-sizeof(nb_yfe2_rx_t))return NULL;
    } while(!atomic_compare_exchange_weak(&global_allocated_bytes,&current,
        current+sizeof(nb_yfe2_rx_t)));
    nb_yfe2_rx_t* state=calloc(1,sizeof(*state));
    if(state==NULL)atomic_fetch_sub(&global_allocated_bytes,sizeof(nb_yfe2_rx_t));
    return state;
}
void nb_yfe2_rx_destroy(nb_yfe2_rx_t* state){
    if(state==NULL)return;
    size_t active=0;
    for(size_t i=0;i<NB_YFE2_RX_FLOW_BLOCKS;i++)active+=state->blocks[i].valid!=0;
    if(active)atomic_fetch_sub(&global_active_blocks,active);
    free(state);atomic_fetch_sub(&global_allocated_bytes,sizeof(nb_yfe2_rx_t));
}
int nb_yfe2_rx_note_source(nb_yfe2_rx_t* state,uint64_t generation,const nb_udp_wire_view_t* source,uint64_t now_us){
    if(state==NULL||generation==0||source==NULL||source->type!=NB_UDP_TYPE_C2S||source->session_id==0||
        source->payload==NULL||source->payload_length==0||source->payload_length>NB_UDP_FRAGMENT_PAYLOAD)return -1;
    nb_yfe2_desc_t desc={source->sequence,source->fragment_index,source->fragment_count,source->total_length,source->payload_length};
    source_item_t* existing=cache_find(state,generation,source->session_id,&desc);
    if(existing&&existing->delivered){state->metrics.duplicate++;return 1;}
    cache_store(state,generation,source->session_id,&desc,source->payload,now_us,1);return 0;
}
static block_t* block_get(nb_yfe2_rx_t* state,uint64_t generation,const nb_yfe2_parity_view_t* parity,uint64_t now){
    block_t* free_slot=NULL;block_t* oldest=&state->blocks[0];
    for(size_t i=0;i<NB_YFE2_RX_FLOW_BLOCKS;i++){block_t* b=&state->blocks[i];
        if(b->valid&&b->generation==generation&&b->session_id==parity->session_id&&b->block_id==parity->block_id)return b;
        if(!b->valid&&!free_slot)free_slot=b;
        if(b->first_seen<oldest->first_seen)oldest=b;}
    block_t* block=free_slot?free_slot:oldest;
    if(block->valid)state->metrics.evicted++;
    else {
        size_t current=atomic_load(&global_active_blocks);
        do {
            if(current>=NB_YFE2_RX_GLOBAL_BLOCKS){state->metrics.evicted++;return NULL;}
        } while(!atomic_compare_exchange_weak(&global_active_blocks,&current,current+1));
    }
    memset(block,0,sizeof(*block));block->valid=1;block->generation=generation;block->first_seen=now;
    block->session_id=parity->session_id;block->block_id=parity->block_id;block->flags=parity->flags;
    block->r=parity->parity_shards;block->count=parity->actual_data_count;block->shard_size=parity->shard_size;
    memcpy(block->desc,parity->desc,(size_t)block->count*sizeof(block->desc[0]));
    memory_high_update(state);return block;
}
static int block_matches(const block_t* block,const nb_yfe2_parity_view_t* parity){return block->flags==parity->flags&&
    block->r==parity->parity_shards&&block->count==parity->actual_data_count&&block->shard_size==parity->shard_size&&
    !memcmp(block->desc,parity->desc,(size_t)block->count*sizeof(block->desc[0]));}
int nb_yfe2_rx_add_parity(nb_yfe2_rx_t* state,uint64_t generation,const nb_yfe2_parity_view_t* parity,
    const char* route,uint16_t route_length,uint64_t now_us,nb_yfe2_recovered_t* output,size_t output_cap,size_t* output_count){
    if(state==NULL||generation==0||parity==NULL||route==NULL||route_length==0||output==NULL||output_count==NULL||
        route_length>=NB_UDP_ROUTE_MAX||parity->body_length!=parity->shard_size)return -1;
    block_t* block=block_get(state,generation,parity,now_us);if(block==NULL)return 0;
    if(!block_matches(block,parity)){state->metrics.corrupt++;block->valid=0;
        atomic_fetch_sub(&global_active_blocks,1);return -1;}
    uint8_t repair_index=(uint8_t)(parity->shard_index-NB_YFE2_DATA_SHARDS);
    if(block->repair_present[repair_index]){state->metrics.duplicate++;return 0;}
    memcpy(block->repair[repair_index],parity->body,parity->shard_size);block->repair_present[repair_index]=1;
    uint8_t source[16][NB_UDP_FRAGMENT_PAYLOAD]={{0}};uint8_t* src[16];int src_present[16];
    uint8_t* repair[3];int repair_present[3];size_t missing=0,available=0;
    for(size_t i=0;i<16;i++){src[i]=source[i];src_present[i]=i>=block->count;
        if(i<block->count){source_item_t* item=cache_find(state,generation,block->session_id,&block->desc[i]);
            if(item){memcpy(source[i],item->payload,item->desc.payload_length);src_present[i]=1;}else missing++;}
        if(src_present[i])available++;}
    for(uint8_t i=0;i<block->r;i++){repair[i]=block->repair[i];repair_present[i]=block->repair_present[i];if(repair_present[i])available++;}
    if(missing==0){block->valid=0;atomic_fetch_sub(&global_active_blocks,1);return 0;}
    if(available<16)return 0;
    if(missing>block->r)return 0;
    uint64_t decode_started=monotonic_ns();
    int decode_result=nb_rs_recover(16,block->r,src,src_present,repair,repair_present,
        block->shard_size);
    uint64_t decode_finished=monotonic_ns();
    if(decode_finished>=decode_started)state->metrics.decode_ns+=decode_finished-decode_started;
    if(decode_result!=0)return 0;
    if(*output_count+missing>output_cap)return -1;
    for(uint8_t i=0;i<block->count;i++)if(!src_present[i]){
        nb_yfe2_desc_t* d=&block->desc[i];nb_yfe2_recovered_t* item=&output[(*output_count)++];
        int length=nb_udp_wire_encode(item->wire,sizeof(item->wire),NB_UDP_TYPE_C2S,block->session_id,
            d->sequence,d->fragment_index,d->fragment_count,d->total_length,route,route_length,source[i],d->payload_length);
        if(length<0){state->metrics.corrupt++;return -1;}item->length=(size_t)length;
        cache_store(state,generation,block->session_id,d,source[i],now_us,1);state->metrics.recovered++;
    }
    block->valid=0;atomic_fetch_sub(&global_active_blocks,1);return 1;
}
size_t nb_yfe2_rx_expire(nb_yfe2_rx_t* state,uint64_t now_us,nb_yfe2_rx_metrics_t* metrics){
    if(state==NULL)return 0;
    size_t expired=0;
    for(size_t i=0;i<NB_YFE2_RX_FLOW_BLOCKS;i++){block_t* b=&state->blocks[i];
        if(b->valid&&now_us>=b->first_seen&&now_us-b->first_seen>=NB_YFE2_RECOVERY_DEADLINE_US){
            b->valid=0;atomic_fetch_sub(&global_active_blocks,1);
            expired++;state->metrics.unrecoverable++;}}
    if(metrics)*metrics=state->metrics;
    return expired;
}
uint64_t nb_yfe2_rx_next_deadline(const nb_yfe2_rx_t* state){
    uint64_t earliest=0;
    if(state==NULL)return 0;
    for(size_t i=0;i<NB_YFE2_RX_FLOW_BLOCKS;i++){
        const block_t* block=&state->blocks[i];
        if(!block->valid)continue;
        uint64_t deadline=block->first_seen>UINT64_MAX-NB_YFE2_RECOVERY_DEADLINE_US?
            UINT64_MAX:block->first_seen+NB_YFE2_RECOVERY_DEADLINE_US;
        if(earliest==0||deadline<earliest)earliest=deadline;
    }
    return earliest;
}
void nb_yfe2_rx_snapshot(const nb_yfe2_rx_t* state,nb_yfe2_rx_metrics_t* out){if(out){if(state)*out=state->metrics;else memset(out,0,sizeof(*out));}}
