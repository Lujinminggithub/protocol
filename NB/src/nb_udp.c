#include "nb_udp.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_UDP_WIRE_HEADER 24

static void put16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }
static void put32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }
static uint16_t get16(const uint8_t* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t get32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

int nb_udp_wire_type_valid(uint8_t type){
    return type==NB_UDP_TYPE_C2S||type==NB_UDP_TYPE_S2C||
        type==NB_UDP_TYPE_CLOSE||type==NB_UDP_TYPE_CLOSE_ACK;
}

uint16_t nb_udp_fragment_count(size_t payload_length){
    if(payload_length == 0 || payload_length > NB_UDP_MAX_PAYLOAD) return 0;
    return (uint16_t)((payload_length + NB_UDP_FRAGMENT_PAYLOAD - 1) / NB_UDP_FRAGMENT_PAYLOAD);
}

int nb_udp_wire_encode(uint8_t* out, size_t cap, uint8_t type,
    uint32_t session_id, uint32_t sequence, uint16_t fragment_index,
    uint16_t fragment_count, uint16_t total_length, const char* route,
    uint16_t route_length, const uint8_t* payload, uint16_t payload_length){
    if(out==NULL||session_id==0||!nb_udp_wire_type_valid(type)||
        fragment_count==0||fragment_count>66||fragment_index>=fragment_count||
        total_length==0||route==NULL||route_length==0||route_length>=NB_UDP_ROUTE_MAX||
        payload==NULL||payload_length==0||payload_length>NB_UDP_FRAGMENT_PAYLOAD||
        cap<NB_UDP_WIRE_HEADER+(size_t)route_length+payload_length) return -1;
    size_t offset=(size_t)fragment_index*NB_UDP_FRAGMENT_PAYLOAD;
    if(offset+payload_length>total_length) return -1;
    if(fragment_index+1<fragment_count&&payload_length!=NB_UDP_FRAGMENT_PAYLOAD) return -1;
    if(fragment_index+1==fragment_count&&offset+payload_length!=total_length) return -1;
    put32(out,NB_UDP_MAGIC);out[4]=NB_UDP_VERSION;out[5]=type;put16(out+6,NB_UDP_WIRE_HEADER);
    put32(out+8,session_id);put32(out+12,sequence);put16(out+16,fragment_index);
    put16(out+18,fragment_count);put16(out+20,total_length);put16(out+22,route_length);
    memcpy(out+NB_UDP_WIRE_HEADER,route,route_length);
    memcpy(out+NB_UDP_WIRE_HEADER+route_length,payload,payload_length);
    return (int)(NB_UDP_WIRE_HEADER+route_length+payload_length);
}

int nb_udp_wire_decode(const uint8_t* data, size_t length, nb_udp_wire_view_t* out){
    if(data==NULL||out==NULL||length<NB_UDP_WIRE_HEADER||get32(data)!=NB_UDP_MAGIC||
        data[4]!=NB_UDP_VERSION||get16(data+6)!=NB_UDP_WIRE_HEADER) return -1;
    memset(out,0,sizeof(*out));out->type=data[5];out->session_id=get32(data+8);
    out->sequence=get32(data+12);out->fragment_index=get16(data+16);
    out->fragment_count=get16(data+18);out->total_length=get16(data+20);
    out->route_length=get16(data+22);
    if(!nb_udp_wire_type_valid(out->type)||out->session_id==0||
        out->fragment_count==0||out->fragment_count>66||out->fragment_index>=out->fragment_count||
        out->total_length==0||out->route_length==0||out->route_length>=NB_UDP_ROUTE_MAX||
        length<=(size_t)NB_UDP_WIRE_HEADER+out->route_length) return -1;
    size_t payload_length=length-NB_UDP_WIRE_HEADER-out->route_length;
    size_t offset=(size_t)out->fragment_index*NB_UDP_FRAGMENT_PAYLOAD;
    if(payload_length>NB_UDP_FRAGMENT_PAYLOAD||offset+payload_length>out->total_length||
        (out->fragment_index+1<out->fragment_count&&payload_length!=NB_UDP_FRAGMENT_PAYLOAD)||
        (out->fragment_index+1==out->fragment_count&&offset+payload_length!=out->total_length)) return -1;
    out->route=data+NB_UDP_WIRE_HEADER;out->payload=out->route+out->route_length;
    out->payload_length=(uint16_t)payload_length;return 0;
}

void nb_udp_reassembly_init(nb_udp_reassembly_t* state){ if(state)memset(state,0,sizeof(*state)); }
void nb_udp_reassembly_dispose(nb_udp_reassembly_t* state){
    if(state==NULL)return;
    for(size_t i=0;i<NB_UDP_REASSEMBLY_SLOTS;i++)free(state->slots[i].data);
    memset(state,0,sizeof(*state));
}

static nb_udp_reassembly_slot_t* slot_get(nb_udp_reassembly_t* state,
    const nb_udp_wire_view_t* f,uint64_t now_us){
    nb_udp_reassembly_slot_t* candidate=NULL;
    for(size_t i=0;i<NB_UDP_REASSEMBLY_SLOTS;i++){
        nb_udp_reassembly_slot_t* s=&state->slots[i];
        if(s->active&&s->sequence==f->sequence)return s;
        if(!s->active){candidate=s;break;}
        if(candidate==NULL||s->updated_at<candidate->updated_at)candidate=s;
    }
    if(candidate==NULL)candidate=&state->slots[0];
    free(candidate->data);memset(candidate,0,sizeof(*candidate));
    candidate->data=malloc(f->total_length);if(candidate->data==NULL)return NULL;
    candidate->active=1;candidate->sequence=f->sequence;candidate->updated_at=now_us;
    candidate->total_length=f->total_length;candidate->fragment_count=f->fragment_count;
    candidate->route_length=f->route_length;memcpy(candidate->route,f->route,f->route_length);
    candidate->route[f->route_length]=0;return candidate;
}

int nb_udp_reassembly_feed(nb_udp_reassembly_t* state, const nb_udp_wire_view_t* f,
    uint64_t now_us, nb_udp_reassembled_t* out){
    if(state==NULL||f==NULL||out==NULL)return -1;
    memset(out,0,sizeof(*out));
    nb_udp_reassembly_slot_t* s=slot_get(state,f,now_us);if(s==NULL)return -1;
    if(s->total_length!=f->total_length||s->fragment_count!=f->fragment_count||
        s->route_length!=f->route_length||memcmp(s->route,f->route,f->route_length)!=0){
        free(s->data);memset(s,0,sizeof(*s));return -1;
    }
    uint64_t mask=1ULL<<(f->fragment_index&63);uint64_t* word=&s->received[f->fragment_index>>6];
    if((*word&mask)==0){
        memcpy(s->data+(size_t)f->fragment_index*NB_UDP_FRAGMENT_PAYLOAD,f->payload,f->payload_length);
        *word|=mask;s->received_count++;
    }
    s->updated_at=now_us;if(s->received_count!=s->fragment_count)return 0;
    out->payload=s->data;out->payload_length=s->total_length;out->route=s->route;
    out->route_length=s->route_length;out->sequence=s->sequence;s->active=0;return 1;
}

int nb_udp_control_grace_expired(uint64_t control_closed_at, uint64_t last_active,
    uint64_t now_us, uint64_t grace_us){
    if(control_closed_at==0||grace_us==0)return 0;
    uint64_t reference=last_active>control_closed_at?last_active:control_closed_at;
    return now_us>reference&&now_us-reference>=grace_us;
}

int nb_socks_udp_parse(const uint8_t* data, size_t length, char* host, size_t host_cap,
    int* port, const uint8_t** payload, size_t* payload_length){
    if(data==NULL||host==NULL||host_cap==0||port==NULL||payload==NULL||payload_length==NULL||
        length<4||data[0]!=0||data[1]!=0||data[2]!=0) return -1;
    size_t offset=4;
    if(data[3]==1){
        if(length<10||host_cap<16)return -1;
        snprintf(host,host_cap,"%u.%u.%u.%u",data[4],data[5],data[6],data[7]);offset=8;
    }else if(data[3]==3){
        if(length<5)return -1;
        size_t n=data[4];if(n==0||n>=host_cap||length<7+n)return -1;
        memcpy(host,data+5,n);host[n]=0;offset=5+n;
    }else if(data[3]==4){
        if(length<22||inet_ntop(AF_INET6,data+4,host,(socklen_t)host_cap)==NULL)return -1;
        offset=20;
    }else return -1;
    *port=(data[offset]<<8)|data[offset+1];offset+=2;if(*port<=0||offset>=length)return -1;
    *payload=data+offset;*payload_length=length-offset;return 0;
}

int nb_socks_udp_encode(uint8_t* out, size_t cap, const char* host, int port,
    const uint8_t* payload, size_t payload_length){
    if(out==NULL||host==NULL||port<=0||port>65535||payload==NULL||payload_length==0)return -1;
    struct in_addr a4;struct in6_addr a6;size_t offset=0;out[0]=0;out[1]=0;out[2]=0;
    if(inet_pton(AF_INET,host,&a4)==1){if(cap<10+payload_length)return -1;out[3]=1;memcpy(out+4,&a4,4);offset=8;}
    else if(inet_pton(AF_INET6,host,&a6)==1){if(cap<22+payload_length)return -1;out[3]=4;memcpy(out+4,&a6,16);offset=20;}
    else {size_t n=strlen(host);if(n==0||n>255||cap<7+n+payload_length)return -1;out[3]=3;out[4]=(uint8_t)n;memcpy(out+5,host,n);offset=5+n;}
    out[offset]=(uint8_t)(port>>8);out[offset+1]=(uint8_t)port;offset+=2;
    memcpy(out+offset,payload,payload_length);return (int)(offset+payload_length);
}
