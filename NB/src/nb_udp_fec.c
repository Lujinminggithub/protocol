#include "nb_udp_fec.h"

#include <stdlib.h>
#include <string.h>

#define NB_UDP_FEC_HEADER 24u
#define NB_UDP_FEC_DESC 12u
#define NB_UDP_FEC_CACHE 32u
#define NB_UDP_FEC_LOSS_ON_PCT 0.30
#define NB_UDP_FEC_LOSS_OFF_PCT 0.03
#define NB_UDP_FEC_MIN_ON_US 30000000u

typedef struct {
    uint32_t sequence;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t total_length;
    uint16_t payload_length;
} fec_desc_t;

typedef struct {
    int valid;
    uint8_t direction;
    uint32_t session_id;
    fec_desc_t desc;
    uint64_t observed_at;
    uint8_t payload[NB_UDP_FRAGMENT_PAYLOAD];
} fec_cache_t;

struct nb_udp_fec_tx {
    uint8_t k;
    uint8_t count;
    uint8_t direction;
    uint32_t session_id;
    uint32_t group_id;
    uint64_t hold_us;
    uint64_t first_at;
    uint16_t route_length;
    uint16_t parity_length;
    char route[NB_UDP_ROUTE_MAX];
    fec_desc_t desc[NB_UDP_FEC_MAX_K];
    uint8_t parity[NB_UDP_FRAGMENT_PAYLOAD];
};

struct nb_udp_fec_rx {
    fec_cache_t cache[NB_UDP_FEC_CACHE];
    uint32_t next;
};

static void put16(uint8_t* p,uint16_t value){p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value;}
static void put32(uint8_t* p,uint32_t value){p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;}
static uint16_t get16(const uint8_t* p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t* p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}

static int source_valid(const nb_udp_wire_view_t* source){
    return source&&(source->type==NB_UDP_TYPE_C2S||source->type==NB_UDP_TYPE_S2C)&&
        source->session_id&&source->route&&source->route_length&&source->route_length<NB_UDP_ROUTE_MAX&&
        source->payload&&source->payload_length&&source->payload_length<=NB_UDP_FRAGMENT_PAYLOAD;
}

nb_udp_fec_tx_t* nb_udp_fec_tx_create(uint8_t k,uint64_t hold_us){
    if(k<2||k>NB_UDP_FEC_MAX_K||hold_us<100||hold_us>1000000)return NULL;
    nb_udp_fec_tx_t* state=calloc(1,sizeof(*state));
    if(state){state->k=k;state->hold_us=hold_us;}return state;
}

void nb_udp_fec_tx_destroy(nb_udp_fec_tx_t* state){free(state);}

uint64_t nb_udp_fec_tx_next_deadline(const nb_udp_fec_tx_t* state){
    return state&&state->count&&state->first_at?state->first_at+state->hold_us:0;
}

static void tx_reset(nb_udp_fec_tx_t* state){
    uint8_t k=state->k;uint64_t hold=state->hold_us;memset(state,0,sizeof(*state));state->k=k;state->hold_us=hold;
}

static int tx_flush(nb_udp_fec_tx_t* state,uint8_t* out,size_t cap){
    if(state==NULL||out==NULL||state->count<2){if(state)tx_reset(state);return 0;}
    size_t need=NB_UDP_FEC_HEADER+(size_t)state->count*NB_UDP_FEC_DESC+
        state->route_length+state->parity_length;
    if(need>cap){tx_reset(state);return 0;}
    put32(out,NB_UDP_FEC_MAGIC);out[4]=NB_UDP_FEC_VERSION;out[5]=state->direction;
    out[6]=state->count;out[7]=0;put32(out+8,state->session_id);put32(out+12,state->group_id);
    put16(out+16,state->route_length);put16(out+18,state->parity_length);
    put16(out+20,NB_UDP_FEC_DESC);put16(out+22,NB_UDP_FEC_HEADER);
    size_t offset=NB_UDP_FEC_HEADER;
    for(uint8_t i=0;i<state->count;i++){
        const fec_desc_t* d=&state->desc[i];put32(out+offset,d->sequence);
        put16(out+offset+4,d->fragment_index);put16(out+offset+6,d->fragment_count);
        put16(out+offset+8,d->total_length);put16(out+offset+10,d->payload_length);offset+=NB_UDP_FEC_DESC;
    }
    memcpy(out+offset,state->route,state->route_length);offset+=state->route_length;
    memcpy(out+offset,state->parity,state->parity_length);tx_reset(state);return (int)need;
}

int nb_udp_fec_tx_feed(nb_udp_fec_tx_t* state,const nb_udp_wire_view_t* source,
    uint64_t now_us,uint8_t* repair,size_t repair_cap){
    if(state==NULL||!source_valid(source)||repair==NULL)return -1;
    if(state->count&&(
        state->direction!=source->type||state->session_id!=source->session_id||
        state->route_length!=source->route_length||memcmp(state->route,source->route,source->route_length)!=0)){
        tx_reset(state);
    }
    if(state->count==0){
        state->direction=source->type;state->session_id=source->session_id;
        state->group_id=source->sequence;state->first_at=now_us;state->route_length=source->route_length;
        memcpy(state->route,source->route,source->route_length);state->route[source->route_length]=0;
    }
    fec_desc_t* d=&state->desc[state->count];d->sequence=source->sequence;
    d->fragment_index=source->fragment_index;d->fragment_count=source->fragment_count;
    d->total_length=source->total_length;d->payload_length=source->payload_length;
    for(uint16_t i=0;i<source->payload_length;i++)state->parity[i]^=source->payload[i];
    if(source->payload_length>state->parity_length)state->parity_length=source->payload_length;
    state->count++;return state->count==state->k?tx_flush(state,repair,repair_cap):0;
}

int nb_udp_fec_tx_flush_due(nb_udp_fec_tx_t* state,uint64_t now_us,uint8_t* repair,size_t repair_cap){
    if(state==NULL||repair==NULL)return -1;
    uint64_t deadline=nb_udp_fec_tx_next_deadline(state);
    return deadline&&now_us>=deadline?tx_flush(state,repair,repair_cap):0;
}

nb_udp_fec_rx_t* nb_udp_fec_rx_create(void){return calloc(1,sizeof(nb_udp_fec_rx_t));}
void nb_udp_fec_rx_destroy(nb_udp_fec_rx_t* state){free(state);}

static int desc_equal(const fec_desc_t* a,const fec_desc_t* b){
    return a->sequence==b->sequence&&a->fragment_index==b->fragment_index&&
        a->fragment_count==b->fragment_count&&a->total_length==b->total_length&&
        a->payload_length==b->payload_length;
}

int nb_udp_fec_rx_seen(const nb_udp_fec_rx_t* state,const nb_udp_wire_view_t* source){
    if(state==NULL||!source_valid(source))return 0;
    fec_desc_t desc={source->sequence,source->fragment_index,source->fragment_count,
        source->total_length,source->payload_length};
    for(size_t i=0;i<NB_UDP_FEC_CACHE;i++)if(state->cache[i].valid&&
        state->cache[i].direction==source->type&&state->cache[i].session_id==source->session_id&&
        desc_equal(&state->cache[i].desc,&desc))return 1;
    return 0;
}

int nb_udp_fec_rx_note(nb_udp_fec_rx_t* state,const nb_udp_wire_view_t* source,uint64_t now_us){
    if(state==NULL||!source_valid(source))return -1;
    fec_desc_t desc={source->sequence,source->fragment_index,source->fragment_count,
        source->total_length,source->payload_length};
    if(nb_udp_fec_rx_seen(state,source))return 0;
    fec_cache_t* item=&state->cache[state->next++%NB_UDP_FEC_CACHE];memset(item,0,sizeof(*item));
    item->valid=1;item->direction=source->type;item->session_id=source->session_id;
    item->desc=desc;item->observed_at=now_us;memcpy(item->payload,source->payload,source->payload_length);return 0;
}

int nb_udp_fec_peek(const uint8_t* data,size_t length,uint32_t* session_id,uint8_t* direction){
    if(data==NULL||length<NB_UDP_FEC_HEADER||get32(data)!=NB_UDP_FEC_MAGIC||data[4]!=NB_UDP_FEC_VERSION||
        (data[5]!=NB_UDP_TYPE_C2S&&data[5]!=NB_UDP_TYPE_S2C)||data[6]<2||data[6]>NB_UDP_FEC_MAX_K||
        get32(data+8)==0||get16(data+20)!=NB_UDP_FEC_DESC||get16(data+22)!=NB_UDP_FEC_HEADER)return -1;
    if(session_id)*session_id=get32(data+8);
    if(direction)*direction=data[5];
    return 0;
}

int nb_udp_fec_rx_recover(nb_udp_fec_rx_t* state,const uint8_t* repair,size_t repair_len,
    uint64_t now_us,uint8_t* recovered,size_t recovered_cap){
    uint32_t session=0;uint8_t direction=0;
    if(state==NULL||recovered==NULL||nb_udp_fec_peek(repair,repair_len,&session,&direction))return -1;
    uint8_t count=repair[6];uint16_t route_len=get16(repair+16),parity_len=get16(repair+18);
    size_t desc_end=NB_UDP_FEC_HEADER+(size_t)count*NB_UDP_FEC_DESC;
    if(route_len==0||route_len>=NB_UDP_ROUTE_MAX||parity_len==0||parity_len>NB_UDP_FRAGMENT_PAYLOAD||
        desc_end+(size_t)route_len+parity_len!=repair_len)return -1;
    const uint8_t* route=repair+desc_end;const uint8_t* parity=route+route_len;
    uint8_t data[NB_UDP_FRAGMENT_PAYLOAD];memcpy(data,parity,parity_len);
    int missing=-1;fec_desc_t missing_desc={0};
    for(uint8_t i=0;i<count;i++){
        const uint8_t* raw=repair+NB_UDP_FEC_HEADER+(size_t)i*NB_UDP_FEC_DESC;
        fec_desc_t desc={get32(raw),get16(raw+4),get16(raw+6),get16(raw+8),get16(raw+10)};
        if(desc.payload_length==0||desc.payload_length>parity_len||desc.fragment_count==0||
            desc.fragment_index>=desc.fragment_count)return -1;
        fec_cache_t* found=NULL;
        for(size_t j=0;j<NB_UDP_FEC_CACHE;j++)if(state->cache[j].valid&&
            state->cache[j].direction==direction&&state->cache[j].session_id==session&&
            desc_equal(&state->cache[j].desc,&desc)){found=&state->cache[j];break;}
        if(found==NULL){if(missing>=0)return 0;missing=i;missing_desc=desc;continue;}
        for(uint16_t j=0;j<found->desc.payload_length;j++)data[j]^=found->payload[j];
    }
    if(missing<0)return 0;
    char route_text[NB_UDP_ROUTE_MAX];memcpy(route_text,route,route_len);route_text[route_len]=0;
    int length=nb_udp_wire_encode(recovered,recovered_cap,direction,session,missing_desc.sequence,
        missing_desc.fragment_index,missing_desc.fragment_count,missing_desc.total_length,
        route_text,route_len,data,missing_desc.payload_length);
    if(length>0){nb_udp_wire_view_t view;if(nb_udp_wire_decode(recovered,(size_t)length,&view)==0)
        (void)nb_udp_fec_rx_note(state,&view,now_us);}
    return length;
}

int nb_udp_fec_adaptive_update(nb_udp_fec_adaptive_t* state,double loss_pct,
    uint64_t jitter_us,int sample_valid,uint64_t now_us){
    if(state==NULL||loss_pct<0.0)return 0;
    (void)jitter_us;
    if(!sample_valid)return state->active;
    int degraded=loss_pct>NB_UDP_FEC_LOSS_ON_PCT;
    if(degraded){
        state->active=1;state->changed_at=now_us;
    }else if(state->active&&now_us>=state->changed_at&&now_us-state->changed_at>=NB_UDP_FEC_MIN_ON_US&&
        loss_pct<=NB_UDP_FEC_LOSS_OFF_PCT){
        state->active=0;state->changed_at=now_us;
    }
    return state->active;
}
