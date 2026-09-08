#include "nb_yfe2_wire.h"

#include <string.h>

static void put16(uint8_t* p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static void put32(uint8_t* p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint16_t get16(const uint8_t* p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t* p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}

static int params_valid(uint8_t flags,uint8_t direction,uint8_t shard_index,uint8_t k,uint8_t r,uint8_t count,uint16_t size){
    uint8_t expected_r=(flags&NB_YFE2_FLAG_BURST)?NB_YFE2_BURST_PARITY:NB_YFE2_BASE_PARITY;
    return (flags&~NB_YFE2_FLAG_BURST)==0&&direction==NB_YFE2_DIRECTION_C2S&&k==NB_YFE2_DATA_SHARDS&&
        r==expected_r&&count>=1&&count<=k&&shard_index>=k&&shard_index<k+r&&size>0;
}
static int desc_valid(const nb_yfe2_desc_t* d,uint16_t shard_size){return d&&d->fragment_count>0&&
    d->fragment_index<d->fragment_count&&d->total_length>0&&d->payload_length>0&&d->payload_length<=shard_size;}

int nb_yfe2_wire_encode(uint8_t* out,size_t cap,const nb_yfe2_parity_t* v){
    if(out==NULL||v==NULL||v->desc==NULL||v->body==NULL||v->session_id==0||v->profile_id!=NB_YFE2_PROFILE_ID||
        !params_valid(v->flags,v->direction,v->shard_index,v->data_shards,v->parity_shards,v->actual_data_count,v->shard_size))return -1;
    size_t need=NB_YFE2_WIRE_HEADER+(size_t)v->actual_data_count*NB_YFE2_WIRE_DESC+v->shard_size;
    if(need>cap||need>NB_YFE2_MAX_PARITY_WIRE)return -1;
    memset(out,0,NB_YFE2_WIRE_HEADER);memcpy(out,"NBUF",4);out[4]=3;out[5]=v->flags;
    put16(out+6,v->profile_id);put32(out+8,v->session_id);put32(out+12,v->block_id);
    out[16]=v->direction;out[17]=v->shard_index;out[18]=v->data_shards;out[19]=v->parity_shards;
    out[20]=v->actual_data_count;put16(out+22,v->shard_size);size_t offset=NB_YFE2_WIRE_HEADER;
    for(uint8_t i=0;i<v->actual_data_count;i++){
        const nb_yfe2_desc_t* d=&v->desc[i];if(!desc_valid(d,v->shard_size))return -1;
        put32(out+offset,d->sequence);put16(out+offset+4,d->fragment_index);put16(out+offset+6,d->fragment_count);
        put16(out+offset+8,d->total_length);put16(out+offset+10,d->payload_length);offset+=NB_YFE2_WIRE_DESC;
    }
    memcpy(out+offset,v->body,v->shard_size);return (int)need;
}

nb_yfe2_wire_error_t nb_yfe2_wire_decode(const uint8_t* wire,size_t length,nb_yfe2_parity_view_t* out){
    if(wire==NULL||out==NULL)return NB_YFE2_WIRE_ARGUMENT;
    if(length<NB_YFE2_WIRE_HEADER)return NB_YFE2_WIRE_LENGTH;
    if(memcmp(wire,"NBUF",4))return NB_YFE2_WIRE_MAGIC;
    if(wire[4]!=3)return NB_YFE2_WIRE_VERSION;
    if(wire[5]&~NB_YFE2_FLAG_BURST)return NB_YFE2_WIRE_FLAGS;
    if(get16(wire+6)!=NB_YFE2_PROFILE_ID)return NB_YFE2_WIRE_PROFILE;
    if(wire[16]!=NB_YFE2_DIRECTION_C2S)return NB_YFE2_WIRE_DIRECTION;
    if(wire[21]!=0)return NB_YFE2_WIRE_RESERVED;
    if(!params_valid(wire[5],wire[16],wire[17],wire[18],wire[19],wire[20],get16(wire+22))||get32(wire+8)==0)
        return NB_YFE2_WIRE_SHARDS;
    uint16_t shard_size=get16(wire+22);size_t need=NB_YFE2_WIRE_HEADER+(size_t)wire[20]*NB_YFE2_WIRE_DESC+shard_size;
    if(need!=length||need>NB_YFE2_MAX_PARITY_WIRE)return NB_YFE2_WIRE_LENGTH;
    nb_yfe2_parity_view_t value={0};value.flags=wire[5];value.profile_id=get16(wire+6);
    value.session_id=get32(wire+8);value.block_id=get32(wire+12);value.direction=wire[16];
    value.shard_index=wire[17];value.data_shards=wire[18];value.parity_shards=wire[19];
    value.actual_data_count=wire[20];value.shard_size=shard_size;size_t offset=NB_YFE2_WIRE_HEADER;
    for(uint8_t i=0;i<value.actual_data_count;i++){
        nb_yfe2_desc_t d={get32(wire+offset),get16(wire+offset+4),get16(wire+offset+6),
            get16(wire+offset+8),get16(wire+offset+10)};
        if(!desc_valid(&d,shard_size))return NB_YFE2_WIRE_DESCRIPTOR;
        value.desc[i]=d;offset+=NB_YFE2_WIRE_DESC;
    }
    value.body=wire+offset;value.body_length=shard_size;*out=value;return NB_YFE2_WIRE_OK;
}
