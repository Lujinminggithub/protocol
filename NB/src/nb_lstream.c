#include "nb_lstream.h"
#include "nb_tenant_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t* p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static void put64(uint8_t* p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
static uint32_t get32(const uint8_t* p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t get64(const uint8_t* p){uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static void put16(uint8_t* p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static uint16_t get16(const uint8_t* p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}

int nb_lstream_type_valid(uint8_t type){return type>=NB_LSTREAM_OPEN&&type<=NB_LSTREAM_FIN_ACK;}
int nb_lstream_frame_encode(uint8_t* out,size_t cap,uint8_t type,uint64_t flow_hi,
    uint64_t flow_lo,uint64_t offset,const uint8_t* payload,uint32_t payload_length){
    if(out==NULL||(flow_hi==0&&flow_lo==0)||!nb_lstream_type_valid(type)||payload_length>NB_LSTREAM_DATA_MAX||
        (payload_length>0&&payload==NULL)||(type!=NB_LSTREAM_DATA&&payload_length!=0)||
        cap<NB_LSTREAM_HEADER+(size_t)payload_length)return -1;
    put32(out,NB_LSTREAM_MAGIC);out[4]=NB_LSTREAM_VERSION;out[5]=type;put16(out+6,NB_LSTREAM_HEADER);
    put64(out+8,flow_hi);put64(out+16,flow_lo);put64(out+24,offset);put32(out+32,payload_length);
    if(payload_length)memcpy(out+NB_LSTREAM_HEADER,payload,payload_length);
    return (int)(NB_LSTREAM_HEADER+payload_length);
}
int nb_lstream_bootstrap_encode(uint8_t* out,size_t cap,int priority,const char* tenant,const char* route,
    uint8_t type,uint64_t flow_hi,uint64_t flow_lo,uint64_t offset){
    if(out==NULL||route==NULL||priority<=0||priority>255||
        (type!=NB_LSTREAM_OPEN&&type!=NB_LSTREAM_RESUME)||strchr(route,'\n')||strchr(route,'\r'))return -1;
    char flow[33];if(nb_lstream_flow_render(flow,flow_hi,flow_lo)!=0)return -1;
    int header;
    if(tenant&&tenant[0]){
        if(!nb_tenant_wire_name_valid(tenant))return -1;
        header=snprintf((char*)out,cap,"%d;A=%s;L=%s;%s\n",priority,tenant,flow,route);
    }else header=snprintf((char*)out,cap,"%d;L=%s;%s\n",priority,flow,route);
    if(header<=0||(size_t)header>=cap)return -1;
    int frame=nb_lstream_frame_encode(out+(size_t)header,cap-(size_t)header,type,
        flow_hi,flow_lo,offset,NULL,0);
    if(frame<0)return -1;
    return header+frame;
}
int nb_lstream_frame_decode(const uint8_t* wire,size_t length,nb_lstream_frame_t* out){
    if(wire==NULL||out==NULL||length<NB_LSTREAM_HEADER||get32(wire)!=NB_LSTREAM_MAGIC||wire[4]!=NB_LSTREAM_VERSION||
        !nb_lstream_type_valid(wire[5])||get16(wire+6)!=NB_LSTREAM_HEADER)return -1;
    uint32_t payload_length=get32(wire+32);if(payload_length>NB_LSTREAM_DATA_MAX||length!=NB_LSTREAM_HEADER+(size_t)payload_length||
        (wire[5]!=NB_LSTREAM_DATA&&payload_length!=0))return -1;
    memset(out,0,sizeof(*out));out->type=wire[5];out->flow_hi=get64(wire+8);out->flow_lo=get64(wire+16);
    if(out->flow_hi==0&&out->flow_lo==0)return -1;
    out->offset=get64(wire+24);out->payload_length=payload_length;
    out->payload=wire+NB_LSTREAM_HEADER;return 0;
}
void nb_lstream_decoder_init(nb_lstream_decoder_t* decoder){if(decoder)memset(decoder,0,sizeof(*decoder));}
int nb_lstream_decoder_feed(nb_lstream_decoder_t* decoder,const uint8_t* data,size_t length,
    nb_lstream_frame_fn callback,void* ctx){
    if(decoder==NULL||(length>0&&data==NULL)||callback==NULL)return -1;
    size_t offset=0;
    while(offset<length){size_t target=decoder->expected?decoder->expected:NB_LSTREAM_HEADER;
        size_t need=target-decoder->length,part=length-offset;if(part>need)part=need;
        memcpy(decoder->wire+decoder->length,data+offset,part);decoder->length+=part;offset+=part;
        if(decoder->length==NB_LSTREAM_HEADER&&decoder->expected==0){
            if(get32(decoder->wire)!=NB_LSTREAM_MAGIC||decoder->wire[4]!=NB_LSTREAM_VERSION||
                !nb_lstream_type_valid(decoder->wire[5])||get16(decoder->wire+6)!=NB_LSTREAM_HEADER){decoder->length=0;return -1;}
            uint32_t payload=get32(decoder->wire+32);if(payload>NB_LSTREAM_DATA_MAX||
                (decoder->wire[5]!=NB_LSTREAM_DATA&&payload!=0)){decoder->length=0;return -1;}
            decoder->expected=NB_LSTREAM_HEADER+(size_t)payload;}
        if(decoder->expected&&decoder->length==decoder->expected){nb_lstream_frame_t frame;
            if(nb_lstream_frame_decode(decoder->wire,decoder->length,&frame)!=0){decoder->length=decoder->expected=0;return -1;}
            int rc=callback(ctx,&frame);decoder->length=decoder->expected=0;if(rc!=0)return rc;}}
    return 0;
}
void nb_lstream_tx_init(nb_lstream_tx_t* tx,uint64_t flow_hi,uint64_t flow_lo){if(tx){memset(tx,0,sizeof(*tx));tx->flow_hi=flow_hi;tx->flow_lo=flow_lo;}}
void nb_lstream_tx_bind_budget(nb_lstream_tx_t* tx,uint64_t* used,uint64_t limit){if(tx){tx->budget_used=used;tx->budget_limit=limit;}}
static int tx_budget_take(nb_lstream_tx_t* tx,size_t length){
    if(tx->budget_used==NULL||tx->budget_limit==0)return 0;
    if(*tx->budget_used>tx->budget_limit||length>tx->budget_limit-*tx->budget_used)return -1;
    *tx->budget_used+=(uint64_t)length;return 0;
}
static void tx_budget_release(nb_lstream_tx_t* tx,size_t length){
    if(tx->budget_used==NULL)return;
    if((uint64_t)length>=*tx->budget_used)*tx->budget_used=0;
    else *tx->budget_used-=(uint64_t)length;
}
void nb_lstream_tx_dispose(nb_lstream_tx_t* tx){if(tx){tx_budget_release(tx,tx->replay_length);free(tx->replay);memset(tx,0,sizeof(*tx));}}
int nb_lstream_tx_append(nb_lstream_tx_t* tx,const uint8_t* data,size_t length,uint64_t* offset){
    if(tx==NULL||(length>0&&data==NULL)||length>NB_LSTREAM_REPLAY_MAX-tx->replay_length)return -1;
    if(offset)*offset=tx->next_offset;
    if(length==0)return 0;
    if(tx_budget_take(tx,length)!=0)return -1;
    size_t need=tx->replay_length+length;if(need>tx->replay_capacity){size_t cap=tx->replay_capacity?tx->replay_capacity:4096;
        while(cap<need){if(cap>NB_LSTREAM_REPLAY_MAX/2){cap=NB_LSTREAM_REPLAY_MAX;break;}cap*=2;}uint8_t* next=realloc(tx->replay,cap);
        if(next==NULL){tx_budget_release(tx,length);return -1;}
        tx->replay=next;tx->replay_capacity=cap;}
    memcpy(tx->replay+tx->replay_length,data,length);tx->replay_length+=length;tx->next_offset+=length;return 0;
}
int nb_lstream_tx_ack(nb_lstream_tx_t* tx,uint64_t offset){
    if(tx==NULL||offset>tx->next_offset)return -1;
    if(offset<=tx->base_offset)return 0;
    uint64_t consumed64=offset-tx->base_offset;
    if(consumed64>tx->replay_length)return -1;
    size_t consumed=(size_t)consumed64;
    memmove(tx->replay,tx->replay+consumed,tx->replay_length-consumed);
    tx->replay_length-=consumed;tx_budget_release(tx,consumed);tx->base_offset=offset;return 0;
}
int nb_lstream_tx_fin(nb_lstream_tx_t* tx){if(tx==NULL)return -1;tx->fin_queued=1;tx->fin_offset=tx->next_offset;return 0;}
int nb_lstream_flow_render(char out[33],uint64_t hi,uint64_t lo){static const char hex[]="0123456789abcdef";
    if(out==NULL||(hi==0&&lo==0))return -1;
    for(int i=0;i<16;i++){uint8_t b=(uint8_t)(i<8?hi>>(56-i*8):lo>>(56-(i-8)*8));
        out[i*2]=hex[b>>4];out[i*2+1]=hex[b&15];}out[32]=0;return 0;}
static int unhex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
int nb_lstream_flow_parse(const char* text,uint64_t* hi,uint64_t* lo){if(text==NULL||hi==NULL||lo==NULL||strlen(text)!=32)return -1;
    uint64_t a=0,b=0;for(int i=0;i<32;i++){int v=unhex(text[i]);if(v<0)return -1;if(i<16)a=(a<<4)|(unsigned)v;else b=(b<<4)|(unsigned)v;}
    if(a==0&&b==0)return -1;
    *hi=a;*lo=b;return 0;}
int nb_lstream_reset_requires_resume(int is_entry,int logical_endpoint,uint64_t remote_error){
    return is_entry&&logical_endpoint&&remote_error==NB_LSTREAM_RELAY_RESTART_ERROR;
}
int nb_lstream_stop_requires_reset_wait(int is_entry,int logical_endpoint){
    return is_entry&&logical_endpoint;
}
uint64_t nb_lstream_reconnect_delay_us(unsigned failures){
    uint64_t delay=250000ULL;
    while(failures--&&delay<5000000ULL)delay*=2;
    return delay>5000000ULL?5000000ULL:delay;
}
