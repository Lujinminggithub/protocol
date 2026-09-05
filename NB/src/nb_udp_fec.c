#include "nb_udp_fec.h"
#include "nb_fec_rs.h"

#include <stdlib.h>
#include <string.h>

#define NB_UDP_FEC_HEADER 26u
#define NB_UDP_FEC_DESC 12u
#define NB_UDP_FEC_CACHE 128u
#define NB_UDP_FEC_GROUPS 16u
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

typedef struct {
    int valid;
    uint8_t direction;
    uint8_t count;
    uint8_t repair_count;
    uint32_t session_id;
    uint32_t group_id;
    uint16_t route_length;
    uint64_t observed_at;
    char route[NB_UDP_ROUTE_MAX];
    fec_desc_t desc[NB_UDP_FEC_MAX_K];
    uint8_t repair_present[NB_UDP_FEC_MAX_R];
    uint8_t repair[NB_UDP_FEC_MAX_R][NB_UDP_FRAGMENT_PAYLOAD];
} fec_group_t;

struct nb_udp_fec_tx {
    uint8_t k;
    uint8_t count;
    uint8_t direction;
    uint8_t repair_count;
    uint8_t repair_next;
    uint32_t session_id;
    uint32_t group_id;
    uint64_t hold_us;
    uint64_t first_at;
    uint16_t route_length;
    uint16_t parity_length;
    char route[NB_UDP_ROUTE_MAX];
    fec_desc_t desc[NB_UDP_FEC_MAX_K];
    uint8_t source[NB_UDP_FEC_MAX_K][NB_UDP_FRAGMENT_PAYLOAD];
    uint8_t parity[NB_UDP_FEC_MAX_R][NB_UDP_FRAGMENT_PAYLOAD];
};

struct nb_udp_fec_rx {
    fec_cache_t cache[NB_UDP_FEC_CACHE];
    fec_group_t groups[NB_UDP_FEC_GROUPS];
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
    if(state){state->k=k;state->hold_us=hold_us;state->repair_count=NB_UDP_FEC_MAX_R;state->repair_next=NB_UDP_FEC_MAX_R;}
    return state;
}

void nb_udp_fec_tx_destroy(nb_udp_fec_tx_t* state){free(state);}

uint64_t nb_udp_fec_tx_next_deadline(const nb_udp_fec_tx_t* state){
    return state&&state->count&&state->first_at&&state->repair_next>=state->repair_count?
        state->first_at+state->hold_us:0;
}

static void tx_reset(nb_udp_fec_tx_t* state){
    uint8_t k=state->k;uint64_t hold=state->hold_us;uint8_t r=state->repair_count;
    memset(state,0,sizeof(*state));state->k=k;state->hold_us=hold;state->repair_count=r;state->repair_next=r;
}

static int tx_emit_next(nb_udp_fec_tx_t* state,uint8_t* out,size_t cap){
    if(state==NULL||out==NULL||state->repair_next>=state->repair_count)return 0;
    size_t need=NB_UDP_FEC_HEADER+(size_t)state->count*NB_UDP_FEC_DESC+state->route_length+state->parity_length;
    if(need>cap){tx_reset(state);return 0;}
    put32(out,NB_UDP_FEC_MAGIC);out[4]=NB_UDP_FEC_VERSION;out[5]=state->direction;
    out[6]=state->count;out[7]=state->repair_count;put32(out+8,state->session_id);put32(out+12,state->group_id);
    put16(out+16,state->route_length);put16(out+18,state->parity_length);put16(out+20,NB_UDP_FEC_DESC);put16(out+22,NB_UDP_FEC_HEADER);
    out[24]=state->repair_next;out[25]=0;size_t offset=NB_UDP_FEC_HEADER;
    for(uint8_t i=0;i<state->count;i++){
        const fec_desc_t* d=&state->desc[i];put32(out+offset,d->sequence);put16(out+offset+4,d->fragment_index);
        put16(out+offset+6,d->fragment_count);put16(out+offset+8,d->total_length);put16(out+offset+10,d->payload_length);offset+=NB_UDP_FEC_DESC;
    }
    memcpy(out+offset,state->route,state->route_length);offset+=state->route_length;
    memcpy(out+offset,state->parity[state->repair_next],state->parity_length);state->repair_next++;
    if(state->repair_next>=state->repair_count)tx_reset(state);
    return (int)need;
}

static int tx_encode_pending(nb_udp_fec_tx_t* state){
    if(state==NULL||state->count<2)return 0;
    uint8_t* src[NB_UDP_FEC_MAX_K];uint8_t* rep[NB_UDP_FEC_MAX_R];
    for(uint8_t i=0;i<state->count;i++)src[i]=state->source[i];
    for(uint8_t r=0;r<state->repair_count;r++){memset(state->parity[r],0,sizeof(state->parity[r]));rep[r]=state->parity[r];}
    if(nb_rs_encode(state->count,state->repair_count,src,rep,NB_UDP_FRAGMENT_PAYLOAD)!=0)return -1;
    state->parity_length=NB_UDP_FRAGMENT_PAYLOAD;state->repair_next=0;return 0;
}

int nb_udp_fec_tx_feed(nb_udp_fec_tx_t* state,const nb_udp_wire_view_t* source,uint64_t now_us,uint8_t* repair,size_t repair_cap){
    if(state==NULL||!source_valid(source)||repair==NULL)return -1;
    if(state->repair_next<state->repair_count)return -2;
    if(state->count&&(state->direction!=source->type||state->session_id!=source->session_id||state->route_length!=source->route_length||memcmp(state->route,source->route,source->route_length)!=0))tx_reset(state);
    if(state->count==0){state->direction=source->type;state->session_id=source->session_id;state->group_id=source->sequence;state->first_at=now_us;state->route_length=source->route_length;memcpy(state->route,source->route,source->route_length);state->route[source->route_length]=0;}
    if(state->count>=NB_UDP_FEC_MAX_K)return -1;
    fec_desc_t* d=&state->desc[state->count];d->sequence=source->sequence;d->fragment_index=source->fragment_index;d->fragment_count=source->fragment_count;d->total_length=source->total_length;d->payload_length=source->payload_length;
    memset(state->source[state->count],0,sizeof(state->source[state->count]));memcpy(state->source[state->count],source->payload,source->payload_length);
    state->count++;if(state->count!=state->k)return 0;
    if(tx_encode_pending(state)!=0){tx_reset(state);return -1;}return tx_emit_next(state,repair,repair_cap);
}

int nb_udp_fec_tx_next_repair(nb_udp_fec_tx_t* state,uint8_t* repair,size_t repair_cap){return tx_emit_next(state,repair,repair_cap);}

int nb_udp_fec_tx_flush_due(nb_udp_fec_tx_t* state,uint64_t now_us,uint8_t* repair,size_t repair_cap){
    if(state==NULL||repair==NULL)return -1;
    if(state->repair_next<state->repair_count)return tx_emit_next(state,repair,repair_cap);
    uint64_t deadline=nb_udp_fec_tx_next_deadline(state);if(!deadline||now_us<deadline)return 0;
    if(state->count<2){tx_reset(state);return 0;}if(tx_encode_pending(state)!=0){tx_reset(state);return -1;}return tx_emit_next(state,repair,repair_cap);
}

nb_udp_fec_rx_t* nb_udp_fec_rx_create(void){return calloc(1,sizeof(nb_udp_fec_rx_t));}
void nb_udp_fec_rx_destroy(nb_udp_fec_rx_t* state){free(state);}

static int desc_equal(const fec_desc_t* a,const fec_desc_t* b){return a->sequence==b->sequence&&a->fragment_index==b->fragment_index&&a->fragment_count==b->fragment_count&&a->total_length==b->total_length&&a->payload_length==b->payload_length;}

int nb_udp_fec_rx_seen(const nb_udp_fec_rx_t* state,const nb_udp_wire_view_t* source){
    if(state==NULL||!source_valid(source))return 0;
    fec_desc_t desc={source->sequence,source->fragment_index,source->fragment_count,source->total_length,source->payload_length};
    for(size_t i=0;i<NB_UDP_FEC_CACHE;i++){
        if(state->cache[i].valid&&state->cache[i].direction==source->type&&state->cache[i].session_id==source->session_id&&desc_equal(&state->cache[i].desc,&desc))return 1;
    }
    return 0;
}

int nb_udp_fec_rx_note(nb_udp_fec_rx_t* state,const nb_udp_wire_view_t* source,uint64_t now_us){
    if(state==NULL||!source_valid(source))return -1;
    fec_desc_t desc={source->sequence,source->fragment_index,source->fragment_count,source->total_length,source->payload_length};
    if(nb_udp_fec_rx_seen(state,source))return 0;
    fec_cache_t* item=&state->cache[state->next++%NB_UDP_FEC_CACHE];memset(item,0,sizeof(*item));item->valid=1;item->direction=source->type;item->session_id=source->session_id;item->desc=desc;item->observed_at=now_us;memcpy(item->payload,source->payload,source->payload_length);return 0;
}

int nb_udp_fec_peek(const uint8_t* data,size_t length,uint32_t* session_id,uint8_t* direction){
    if(data==NULL||length<NB_UDP_FEC_HEADER||get32(data)!=NB_UDP_FEC_MAGIC||data[4]!=NB_UDP_FEC_VERSION||(data[5]!=NB_UDP_TYPE_C2S&&data[5]!=NB_UDP_TYPE_S2C)||data[6]<2||data[6]>NB_UDP_FEC_MAX_K||data[7]==0||data[7]>NB_UDP_FEC_MAX_R||data[24]>=data[7]||get32(data+8)==0||get16(data+20)!=NB_UDP_FEC_DESC||get16(data+22)!=NB_UDP_FEC_HEADER)return -1;
    if(session_id)*session_id=get32(data+8);
    if(direction)*direction=data[5];
    return 0;
}

static fec_group_t* group_get(nb_udp_fec_rx_t* state,uint32_t sid,uint8_t direction,uint32_t group,uint64_t now){
    fec_group_t* free_slot=NULL;fec_group_t* oldest=&state->groups[0];for(size_t i=0;i<NB_UDP_FEC_GROUPS;i++){fec_group_t* g=&state->groups[i];if(g->valid&&g->session_id==sid&&g->direction==direction&&g->group_id==group)return g;if(!g->valid&&!free_slot)free_slot=g;if(g->observed_at<oldest->observed_at)oldest=g;}fec_group_t* g=free_slot?free_slot:oldest;memset(g,0,sizeof(*g));g->valid=1;g->session_id=sid;g->direction=direction;g->group_id=group;g->observed_at=now;return g;
}

int nb_udp_fec_rx_recover(nb_udp_fec_rx_t* state,const uint8_t* repair,size_t repair_len,uint64_t now_us,uint8_t* recovered,size_t recovered_cap){
    uint32_t sid=0;uint8_t direction=0;if(state==NULL||recovered==NULL||nb_udp_fec_peek(repair,repair_len,&sid,&direction))return -1;
    uint8_t count=repair[6],r_count=repair[7],r_index=repair[24];uint32_t group_id=get32(repair+12);uint16_t route_len=get16(repair+16),parity_len=get16(repair+18);size_t desc_end=NB_UDP_FEC_HEADER+(size_t)count*NB_UDP_FEC_DESC;
    if(route_len==0||route_len>=NB_UDP_ROUTE_MAX||parity_len!=NB_UDP_FRAGMENT_PAYLOAD||desc_end+(size_t)route_len+parity_len!=repair_len)return -1;
    fec_group_t* group=group_get(state,sid,direction,group_id,now_us);group->observed_at=now_us;
    if(group->count==0){group->count=count;group->repair_count=r_count;group->route_length=route_len;memcpy(group->route,repair+desc_end,route_len);group->route[route_len]=0;for(uint8_t i=0;i<count;i++){const uint8_t* raw=repair+NB_UDP_FEC_HEADER+(size_t)i*NB_UDP_FEC_DESC;group->desc[i]=(fec_desc_t){get32(raw),get16(raw+4),get16(raw+6),get16(raw+8),get16(raw+10)};}}
    if(group->count!=count||group->repair_count!=r_count||group->route_length!=route_len||memcmp(group->route,repair+desc_end,route_len)!=0)return -1;
    memcpy(group->repair[r_index],repair+desc_end+route_len,parity_len);group->repair_present[r_index]=1;
    uint8_t src_data[NB_UDP_FEC_MAX_K][NB_UDP_FRAGMENT_PAYLOAD];uint8_t* src[NB_UDP_FEC_MAX_K];int src_present[NB_UDP_FEC_MAX_K];uint8_t* rep[NB_UDP_FEC_MAX_R];int rep_present[NB_UDP_FEC_MAX_R];size_t available=0;
    for(uint8_t i=0;i<count;i++){src[i]=src_data[i];memset(src_data[i],0,sizeof(src_data[i]));src_present[i]=0;for(size_t j=0;j<NB_UDP_FEC_CACHE;j++)if(state->cache[j].valid&&state->cache[j].direction==direction&&state->cache[j].session_id==sid&&desc_equal(&state->cache[j].desc,&group->desc[i])){memcpy(src_data[i],state->cache[j].payload,state->cache[j].desc.payload_length);src_present[i]=1;available++;break;}}
    for(uint8_t r=0;r<r_count;r++){rep[r]=group->repair[r];rep_present[r]=group->repair_present[r];if(rep_present[r])available++;}if(available<count)return 0;
    uint8_t missing=count;for(uint8_t i=0;i<count;i++)if(!src_present[i]){missing=i;break;}if(missing==count)return 0;if(nb_rs_recover(count,r_count,src,src_present,rep,rep_present,NB_UDP_FRAGMENT_PAYLOAD)!=0)return 0;
    char route_text[NB_UDP_ROUTE_MAX];memcpy(route_text,group->route,group->route_length);route_text[group->route_length]=0;const fec_desc_t* d=&group->desc[missing];int length=nb_udp_wire_encode(recovered,recovered_cap,direction,sid,d->sequence,d->fragment_index,d->fragment_count,d->total_length,route_text,group->route_length,src_data[missing],d->payload_length);
    if(length>0){nb_udp_wire_view_t view;if(nb_udp_wire_decode(recovered,(size_t)length,&view)==0)(void)nb_udp_fec_rx_note(state,&view,now_us);}return length;
}

int nb_udp_fec_adaptive_update(nb_udp_fec_adaptive_t* state,double loss_pct,uint64_t jitter_us,int sample_valid,uint64_t now_us){
    if(state==NULL||loss_pct<0.0)return 0;
    (void)jitter_us;
    if(!sample_valid)return state->active;
    int degraded=loss_pct>NB_UDP_FEC_LOSS_ON_PCT;
    if(degraded){state->active=1;state->changed_at=now_us;}
    else if(state->active&&now_us>=state->changed_at&&now_us-state->changed_at>=NB_UDP_FEC_MIN_ON_US&&loss_pct<=NB_UDP_FEC_LOSS_OFF_PCT){state->active=0;state->changed_at=now_us;}
    return state->active;
}
