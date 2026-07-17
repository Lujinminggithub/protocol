#include "nb_fec.h"
#include "nb_fec_rs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_FEC_MAGIC 0x4E424644u
#define NB_FEC_WIRE_VERSION NB_FEC_PROTOCOL_VERSION
#define NB_FEC_DGRAM_SOURCE 3u
#define NB_FEC_DGRAM_REPAIR 4u
#define NB_FEC_DGRAM_HEADER 20u
#define NB_FEC_MIN_HISTORY 16u
#define NB_FEC_MAX_HISTORY 4096u
#define NB_FEC_MIN_WINDOW 16u
#define NB_FEC_MAX_WINDOW 4096u

typedef struct {
    uint32_t block_id;
    uint16_t raw_bytes;
    uint8_t src_count;
    uint8_t acked;
    uint16_t src_len[NB_FEC_MAX_K];
    uint8_t src_data[NB_FEC_MAX_K][NB_FEC_SYMBOL_SIZE];
} nb_fec_tx_block_t;

typedef struct {
    uint32_t block_id;
    uint64_t first_us;
    uint64_t last_us;
    uint64_t last_nack_us;
    uint16_t raw_bytes;
    uint8_t src_count;
    uint8_t nack_count;
    uint8_t src_present[NB_FEC_MAX_K];
    uint8_t repair_present[NB_FEC_MAX_R];
    uint16_t src_len[NB_FEC_MAX_K];
    uint8_t src_data[NB_FEC_MAX_K][NB_FEC_SYMBOL_SIZE];
    uint8_t repair_data[NB_FEC_MAX_R][NB_FEC_SYMBOL_SIZE];
} nb_fec_rx_block_t;

struct nb_fec_session {
    uint32_t session_id;
    nb_fec_config_t cfg;
    nb_fec_callbacks_t cb;
    void* cb_ctx;
    nb_fec_metrics_t metrics;

    uint32_t tx_block_id;
    uint8_t tx_src_count;
    uint16_t tx_raw_bytes;
    uint16_t tx_len[NB_FEC_MAX_K];
    uint8_t tx_data[NB_FEC_MAX_K][NB_FEC_SYMBOL_SIZE];
    uint64_t tx_first_us;
    nb_fec_tx_block_t** tx_history;

    uint32_t rx_next_block;
    nb_fec_rx_block_t** rx_window;
    int rx_fin_known;
    uint32_t rx_final_exclusive;
    int rx_fin_delivered;
    int tx_fin_sent;
    int tx_fin_acked;
    int failed;
};

static void put_u16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void put_u32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint16_t get_u16(const uint8_t* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t get_u32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static uint32_t clamp_u32(uint64_t v, uint32_t low, uint32_t high){
    if(v < low) return low;
    if(v > high) return high;
    return (uint32_t)v;
}

int nb_fec_config_for_bdp(nb_fec_config_t* cfg, uint8_t k, uint8_t r,
    uint64_t bitrate_bps, uint64_t rtt_us, unsigned multiplier)
{
    if(cfg == NULL || k == 0 || k > NB_FEC_MAX_K || r == 0 || r > NB_FEC_MAX_R || multiplier == 0) return NB_FEC_ERR_ARGUMENT;
    memset(cfg, 0, sizeof(*cfg));
    cfg->k = k;
    cfg->r = r;
    cfg->symbol_size = NB_FEC_SYMBOL_SIZE;
    uint64_t block_bytes = (uint64_t)k * NB_FEC_SYMBOL_SIZE;
    uint64_t bdp_bytes = bitrate_bps > 0 && rtt_us > 0
        ? (bitrate_bps / 8u) * rtt_us / 1000000u : 512u * 1024u;
    uint64_t blocks = (bdp_bytes * multiplier + block_bytes - 1u) / block_bytes;
    cfg->tx_history_blocks = clamp_u32(blocks + 8u, NB_FEC_MIN_HISTORY, NB_FEC_MAX_HISTORY);
    cfg->rx_window_blocks = clamp_u32(blocks + 32u, NB_FEC_MIN_WINDOW, NB_FEC_MAX_WINDOW);
    cfg->block_hold_us = 5000u;
    cfg->nack_delay_us = 10000u;
    cfg->nack_retry_us = rtt_us > 0 ? rtt_us : 200000u;
    if(cfg->nack_retry_us < 20000u) cfg->nack_retry_us = 20000u;
    cfg->max_nack_retries = 5u;
    return NB_FEC_OK;
}

static int config_valid(const nb_fec_config_t* c){
    return c != NULL && c->k > 0 && c->k <= NB_FEC_MAX_K && c->r > 0 && c->r <= NB_FEC_MAX_R &&
        c->symbol_size == NB_FEC_SYMBOL_SIZE && c->tx_history_blocks >= NB_FEC_MIN_HISTORY &&
        c->tx_history_blocks <= NB_FEC_MAX_HISTORY && c->rx_window_blocks >= NB_FEC_MIN_WINDOW &&
        c->rx_window_blocks <= NB_FEC_MAX_WINDOW && c->max_nack_retries > 0;
}

nb_fec_session_t* nb_fec_session_create(uint32_t session_id,
    const nb_fec_config_t* cfg, const nb_fec_callbacks_t* cb, void* cb_ctx)
{
    if(session_id == 0 || !config_valid(cfg) || cb == NULL || cb->send_datagram == NULL || cb->send_control == NULL || cb->deliver == NULL) return NULL;
    nb_fec_session_t* s = calloc(1, sizeof(*s));
    if(s == NULL) return NULL;
    s->session_id = session_id;
    s->cfg = *cfg;
    s->cb = *cb;
    s->cb_ctx = cb_ctx;
    s->tx_history = calloc(cfg->tx_history_blocks, sizeof(*s->tx_history));
    s->rx_window = calloc(cfg->rx_window_blocks, sizeof(*s->rx_window));
    if(s->tx_history == NULL || s->rx_window == NULL){ nb_fec_session_destroy(s); return NULL; }
    return s;
}

void nb_fec_session_destroy(nb_fec_session_t* s){
    if(s == NULL) return;
    if(s->tx_history != NULL){
        for(uint32_t i=0;i<s->cfg.tx_history_blocks;i++) free(s->tx_history[i]);
    }
    if(s->rx_window != NULL){
        for(uint32_t i=0;i<s->cfg.rx_window_blocks;i++) free(s->rx_window[i]);
    }
    free(s->tx_history);
    free(s->rx_window);
    free(s);
}

static int send_controlf(nb_fec_session_t* s, const char* fmt, ...){
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if(n <= 0 || (size_t)n >= sizeof(line)) return NB_FEC_ERR_PROTOCOL;
    return s->cb.send_control(s->cb_ctx, (const uint8_t*)line, (size_t)n) == 0 ? NB_FEC_OK : NB_FEC_ERR_IO;
}

static int build_datagram(uint8_t* out, size_t cap, uint8_t type, uint32_t sid,
    uint32_t block_id, uint8_t symbol_idx, uint8_t src_count, uint16_t payload_len,
    uint16_t raw_bytes, const uint8_t* payload)
{
    if(out == NULL || payload == NULL || cap < NB_FEC_DGRAM_HEADER + payload_len) return NB_FEC_ERR_ARGUMENT;
    put_u32(out, NB_FEC_MAGIC); out[4]=NB_FEC_WIRE_VERSION; out[5]=type;
    put_u32(out+6,sid); put_u32(out+10,block_id); out[14]=symbol_idx; out[15]=src_count;
    put_u16(out+16,payload_len); put_u16(out+18,raw_bytes);
    memcpy(out+NB_FEC_DGRAM_HEADER,payload,payload_len);
    return (int)(NB_FEC_DGRAM_HEADER + payload_len);
}

static int block_shape_valid(const nb_fec_session_t* s, uint8_t src_count, uint16_t raw_bytes){
    if(src_count == 0 || src_count > s->cfg.k) return 0;
    uint32_t min_raw = (uint32_t)(src_count - 1u) * s->cfg.symbol_size + 1u;
    uint32_t max_raw = (uint32_t)src_count * s->cfg.symbol_size;
    return raw_bytes >= min_raw && raw_bytes <= max_raw;
}

static uint16_t expected_src_len(const nb_fec_session_t* s, uint8_t src_count,
    uint16_t raw_bytes, uint8_t idx)
{
    if(idx + 1u < src_count) return s->cfg.symbol_size;
    return (uint16_t)(raw_bytes - (uint16_t)((src_count - 1u) * s->cfg.symbol_size));
}

static int history_store(nb_fec_session_t* s, nb_fec_tx_block_t* b){
    uint32_t slot = b->block_id % s->cfg.tx_history_blocks;
    nb_fec_tx_block_t* old = s->tx_history[slot];
    if(old != NULL && !old->acked){
        s->metrics.window_errors++;
        return NB_FEC_ERR_WINDOW;
    }
    free(old);
    s->tx_history[slot] = b;
    return NB_FEC_OK;
}

static nb_fec_tx_block_t* history_find(nb_fec_session_t* s, uint32_t block_id){
    nb_fec_tx_block_t* b = s->tx_history[block_id % s->cfg.tx_history_blocks];
    return b != NULL && b->block_id == block_id ? b : NULL;
}

static int flush_tx_block(nb_fec_session_t* s){
    if(s->tx_raw_bytes == 0) return NB_FEC_OK;
    uint8_t count = s->tx_src_count;
    if(count < s->cfg.k && s->tx_len[count] > 0) count++;
    if(!block_shape_valid(s, count, s->tx_raw_bytes)) return NB_FEC_ERR_PROTOCOL;

    nb_fec_tx_block_t* hb = calloc(1, sizeof(*hb));
    if(hb == NULL) return NB_FEC_ERR_MEMORY;
    hb->block_id=s->tx_block_id; hb->raw_bytes=s->tx_raw_bytes; hb->src_count=count;
    uint8_t* src[NB_FEC_MAX_K]; uint8_t repair[NB_FEC_MAX_R][NB_FEC_SYMBOL_SIZE]; uint8_t* rep[NB_FEC_MAX_R];
    for(uint8_t i=0;i<count;i++){
        hb->src_len[i]=s->tx_len[i];
        memcpy(hb->src_data[i],s->tx_data[i],s->tx_len[i]);
        src[i]=s->tx_data[i];
    }
    for(uint8_t r=0;r<s->cfg.r;r++){ memset(repair[r],0,sizeof(repair[r])); rep[r]=repair[r]; }
    if(nb_rs_encode(count,s->cfg.r,src,rep,s->cfg.symbol_size)!=0){ free(hb); return NB_FEC_ERR_PROTOCOL; }
    int rc=history_store(s,hb); if(rc!=NB_FEC_OK){ free(hb); return rc; }
    rc=send_controlf(s,"FC:BM:%u:%u:%u:%u\n",s->session_id,s->tx_block_id,count,s->tx_raw_bytes);
    if(rc!=NB_FEC_OK) return rc;
    uint8_t wire[NB_FEC_DGRAM_HEADER+NB_FEC_SYMBOL_SIZE];
    for(uint8_t i=0;i<count;i++){
        int n=build_datagram(wire,sizeof(wire),NB_FEC_DGRAM_SOURCE,s->session_id,s->tx_block_id,i,count,s->tx_len[i],s->tx_raw_bytes,s->tx_data[i]);
        if(n<0 || s->cb.send_datagram(s->cb_ctx,wire,(size_t)n)!=0) return NB_FEC_ERR_IO;
        s->metrics.source_packets++; s->metrics.source_bytes+=s->tx_len[i];
    }
    for(uint8_t r=0;r<s->cfg.r;r++){
        int n=build_datagram(wire,sizeof(wire),NB_FEC_DGRAM_REPAIR,s->session_id,s->tx_block_id,r,count,s->cfg.symbol_size,s->tx_raw_bytes,repair[r]);
        if(n<0 || s->cb.send_datagram(s->cb_ctx,wire,(size_t)n)!=0) return NB_FEC_ERR_IO;
        s->metrics.repair_packets++; s->metrics.repair_bytes+=s->cfg.symbol_size;
    }
    s->metrics.blocks_encoded++;
    s->tx_block_id++;
    s->tx_src_count=0; s->tx_raw_bytes=0; s->tx_first_us=0;
    memset(s->tx_len,0,sizeof(s->tx_len)); memset(s->tx_data,0,sizeof(s->tx_data));
    return NB_FEC_OK;
}

int nb_fec_tx_feed(nb_fec_session_t* s, const uint8_t* data, size_t len, int fin, uint64_t now_us){
    if(s == NULL || s->failed || (len > 0 && data == NULL) || s->tx_fin_sent) return NB_FEC_ERR_ARGUMENT;
    size_t off=0;
    while(off<len){
        if(s->tx_raw_bytes==0) s->tx_first_us=now_us;
        uint8_t idx=s->tx_src_count;
        size_t room=s->cfg.symbol_size-s->tx_len[idx];
        size_t take=(len-off<room)?len-off:room;
        memcpy(s->tx_data[idx]+s->tx_len[idx],data+off,take);
        s->tx_len[idx]+=(uint16_t)take; s->tx_raw_bytes+=(uint16_t)take; off+=take;
        if(s->tx_len[idx]==s->cfg.symbol_size) s->tx_src_count++;
        if(s->tx_src_count==s->cfg.k){ int rc=flush_tx_block(s); if(rc!=NB_FEC_OK) return rc; }
    }
    if(fin){
        int rc=flush_tx_block(s); if(rc!=NB_FEC_OK) return rc;
        rc=send_controlf(s,"FC:FIN:%u:%u\n",s->session_id,s->tx_block_id);
        if(rc==NB_FEC_OK) s->tx_fin_sent=1;
        return rc;
    }
    return NB_FEC_OK;
}

static nb_fec_rx_block_t* rx_get(nb_fec_session_t* s, uint32_t block_id, uint64_t now_us, int create){
    if(block_id < s->rx_next_block){ s->metrics.duplicate_packets++; return NULL; }
    uint64_t distance=(uint64_t)block_id-s->rx_next_block;
    if(distance>=s->cfg.rx_window_blocks){ s->metrics.window_errors++; return NULL; }
    uint32_t slot=block_id%s->cfg.rx_window_blocks;
    nb_fec_rx_block_t* b=s->rx_window[slot];
    if(b!=NULL && b->block_id!=block_id) return NULL;
    if(b==NULL && create){
        b=calloc(1,sizeof(*b)); if(b==NULL) return NULL;
        b->block_id=block_id; b->first_us=now_us; b->last_us=now_us;
        s->rx_window[slot]=b;
        if(block_id>s->rx_next_block) s->metrics.out_of_order_packets++;
    }
    return b;
}

static int rx_meta(nb_fec_session_t* s, nb_fec_rx_block_t* b, uint8_t src_count, uint16_t raw_bytes){
    if(!block_shape_valid(s,src_count,raw_bytes)) return NB_FEC_ERR_PROTOCOL;
    if(b->src_count!=0 && (b->src_count!=src_count || b->raw_bytes!=raw_bytes)) return NB_FEC_ERR_PROTOCOL;
    b->src_count=src_count; b->raw_bytes=raw_bytes;
    return NB_FEC_OK;
}

static int recover_block(nb_fec_session_t* s, nb_fec_rx_block_t* b, uint64_t* recovered_bytes){
    uint8_t missing=0; size_t available=0;
    for(uint8_t i=0;i<b->src_count;i++){ if(!b->src_present[i]) missing++; else available++; }
    for(uint8_t r=0;r<s->cfg.r;r++) if(b->repair_present[r]) available++;
    if(missing==0){ *recovered_bytes=0; return 1; }
    if(available<b->src_count) return 0;
    uint8_t* src[NB_FEC_MAX_K]; int src_present[NB_FEC_MAX_K];
    uint8_t* rep[NB_FEC_MAX_R]; int rep_present[NB_FEC_MAX_R];
    for(uint8_t i=0;i<b->src_count;i++){ src[i]=b->src_data[i]; src_present[i]=b->src_present[i]; }
    for(uint8_t r=0;r<s->cfg.r;r++){ rep[r]=b->repair_data[r]; rep_present[r]=b->repair_present[r]; }
    if(nb_rs_recover(b->src_count,s->cfg.r,src,src_present,rep,rep_present,s->cfg.symbol_size)!=0) return 0;
    uint64_t rb=0;
    for(uint8_t i=0;i<b->src_count;i++) if(!b->src_present[i]){
        b->src_present[i]=1; b->src_len[i]=expected_src_len(s,b->src_count,b->raw_bytes,i); rb+=b->src_len[i];
    }
    *recovered_bytes=rb;
    return 1;
}

static int deliver_ready(nb_fec_session_t* s){
    for(;;){
        uint32_t slot=s->rx_next_block%s->cfg.rx_window_blocks;
        nb_fec_rx_block_t* b=s->rx_window[slot];
        if(b==NULL || b->block_id!=s->rx_next_block || b->src_count==0) break;
        uint64_t recovered=0; int ready=recover_block(s,b,&recovered); if(ready<=0) break;
        for(uint8_t i=0;i<b->src_count;i++){
            uint16_t expected=expected_src_len(s,b->src_count,b->raw_bytes,i);
            if(!b->src_present[i] || b->src_len[i]!=expected) return NB_FEC_ERR_PROTOCOL;
            if(s->cb.deliver(s->cb_ctx,b->src_data[i],b->src_len[i],0)!=0) return NB_FEC_ERR_IO;
            s->metrics.delivered_bytes+=b->src_len[i];
        }
        if(recovered>0){ s->metrics.blocks_recovered++; s->metrics.recovered_source_bytes+=recovered; }
        s->metrics.blocks_delivered++;
        int rc=send_controlf(s,"FC:BACK:%u:%u\n",s->session_id,b->block_id); if(rc!=NB_FEC_OK) return rc;
        free(b); s->rx_window[slot]=NULL; s->rx_next_block++;
    }
    if(s->rx_fin_known && !s->rx_fin_delivered && s->rx_next_block==s->rx_final_exclusive){
        if(s->cb.deliver(s->cb_ctx,NULL,0,1)!=0) return NB_FEC_ERR_IO;
        int rc=send_controlf(s,"FC:FINACK:%u:%u\n",s->session_id,s->rx_final_exclusive); if(rc!=NB_FEC_OK) return rc;
        s->rx_fin_delivered=1;
    }
    return NB_FEC_OK;
}

int nb_fec_on_datagram(nb_fec_session_t* s, const uint8_t* data, size_t len, uint64_t now_us){
    if(s==NULL || data==NULL || s->failed || len<NB_FEC_DGRAM_HEADER) return NB_FEC_ERR_ARGUMENT;
    if(get_u32(data)!=NB_FEC_MAGIC || data[4]!=NB_FEC_WIRE_VERSION || (data[5]!=NB_FEC_DGRAM_SOURCE && data[5]!=NB_FEC_DGRAM_REPAIR) || get_u32(data+6)!=s->session_id){
        s->metrics.protocol_errors++; return NB_FEC_ERR_PROTOCOL;
    }
    uint8_t type=data[5], idx=data[14], count=data[15]; uint32_t block=get_u32(data+10);
    uint16_t payload_len=get_u16(data+16), raw=get_u16(data+18);
    if(len!=NB_FEC_DGRAM_HEADER+(size_t)payload_len || !block_shape_valid(s,count,raw)){
        s->metrics.protocol_errors++; return NB_FEC_ERR_PROTOCOL;
    }
    if((type==NB_FEC_DGRAM_SOURCE && (idx>=count || payload_len!=expected_src_len(s,count,raw,idx))) ||
       (type==NB_FEC_DGRAM_REPAIR && (idx>=s->cfg.r || payload_len!=s->cfg.symbol_size))){
        s->metrics.protocol_errors++; return NB_FEC_ERR_PROTOCOL;
    }
    nb_fec_rx_block_t* b=rx_get(s,block,now_us,1);
    if(b==NULL){ s->metrics.window_errors++; return block<s->rx_next_block?NB_FEC_OK:NB_FEC_ERR_WINDOW; }
    int rc=rx_meta(s,b,count,raw); if(rc!=NB_FEC_OK){ s->metrics.protocol_errors++; return rc; }
    b->last_us=now_us;
    const uint8_t* payload=data+NB_FEC_DGRAM_HEADER;
    if(type==NB_FEC_DGRAM_SOURCE){
        if(b->src_present[idx]){ s->metrics.duplicate_packets++; return NB_FEC_OK; }
        memcpy(b->src_data[idx],payload,payload_len); b->src_len[idx]=payload_len; b->src_present[idx]=1;
    } else {
        if(b->repair_present[idx]){ s->metrics.duplicate_packets++; return NB_FEC_OK; }
        memcpy(b->repair_data[idx],payload,payload_len); b->repair_present[idx]=1;
    }
    return deliver_ready(s);
}

static size_t hex_encode(char* out, size_t cap, const uint8_t* in, size_t len){
    static const char h[]="0123456789ABCDEF"; if(cap<len*2u+1u) return 0;
    for(size_t i=0;i<len;i++){ out[2*i]=h[in[i]>>4]; out[2*i+1]=h[in[i]&15]; } out[2*len]=0; return 2*len;
}
static int hex_decode(uint8_t* out, size_t cap, const char* in, size_t len){
    if((len&1u)!=0 || len/2u>cap) return -1;
    for(size_t i=0;i<len;i+=2){
        int a=in[i]>='0'&&in[i]<='9'?in[i]-'0':in[i]>='A'&&in[i]<='F'?in[i]-'A'+10:-1;
        int b=in[i+1]>='0'&&in[i+1]<='9'?in[i+1]-'0':in[i+1]>='A'&&in[i+1]<='F'?in[i+1]-'A'+10:-1;
        if(a<0||b<0) return -1;
        out[i/2]=(uint8_t)((a<<4)|b);
    }
    return (int)(len/2u);
}

static int handle_nack(nb_fec_session_t* s, uint32_t block_id, uint32_t mask){
    nb_fec_tx_block_t* b=history_find(s,block_id);
    if(b==NULL) return nb_fec_send_reset(s,2u)==NB_FEC_OK?NB_FEC_ERR_WINDOW:NB_FEC_ERR_IO;
    char hex[NB_FEC_SYMBOL_SIZE*2u+1u], line[NB_FEC_SYMBOL_SIZE*2u+160u];
    for(uint8_t i=0;i<b->src_count;i++) if(mask&(1u<<i)){
        size_t hn=hex_encode(hex,sizeof(hex),b->src_data[i],b->src_len[i]); if(hn==0) return NB_FEC_ERR_PROTOCOL;
        int n=snprintf(line,sizeof(line),"FC:RETX:%u:%u:%u:%u:%u:%u:%s\n",s->session_id,block_id,i,b->src_count,b->raw_bytes,b->src_len[i],hex);
        if(n<=0||(size_t)n>=sizeof(line)||s->cb.send_control(s->cb_ctx,(const uint8_t*)line,(size_t)n)!=0) return NB_FEC_ERR_IO;
        s->metrics.retx_sent++;
    }
    return NB_FEC_OK;
}

int nb_fec_on_control(nb_fec_session_t* s, const char* line, size_t len, uint64_t now_us){
    if(s==NULL||line==NULL||len==0||len>=4096u||s->failed) return NB_FEC_ERR_ARGUMENT;
    char msg[4096]; memcpy(msg,line,len); msg[len]=0;
    while(len>0&&(msg[len-1]=='\n'||msg[len-1]=='\r')) msg[--len]=0;
    uint32_t sid=0,block=0,mask=0,final=0,code=0; unsigned count=0,raw=0,idx=0,pay=0;
    if(sscanf(msg,"FC:BM:%u:%u:%u:%u",&sid,&block,&count,&raw)==4){
        if(sid!=s->session_id||count>255u||raw>65535u) goto protocol_error;
        nb_fec_rx_block_t* b=rx_get(s,block,now_us,1); if(b==NULL) return block<s->rx_next_block?NB_FEC_OK:NB_FEC_ERR_WINDOW;
        int rc=rx_meta(s,b,(uint8_t)count,(uint16_t)raw); if(rc!=NB_FEC_OK) goto protocol_error;
        return deliver_ready(s);
    }
    if(sscanf(msg,"FC:NACK:%u:%u:%u",&sid,&block,&mask)==3){ if(sid!=s->session_id) goto protocol_error; return handle_nack(s,block,mask); }
    if(sscanf(msg,"FC:BACK:%u:%u",&sid,&block)==2){
        if(sid!=s->session_id) goto protocol_error;
        nb_fec_tx_block_t* b=history_find(s,block);
        if(b!=NULL){ b->acked=1; free(b); s->tx_history[block%s->cfg.tx_history_blocks]=NULL; s->metrics.block_acked++; }
        return NB_FEC_OK;
    }
    if(sscanf(msg,"FC:RACK:%u:%u:%u",&sid,&block,&idx)==3){ if(sid!=s->session_id||idx>=s->cfg.k) goto protocol_error; s->metrics.retx_acked++; return NB_FEC_OK; }
    if(sscanf(msg,"FC:FINACK:%u:%u",&sid,&final)==2){ if(sid!=s->session_id||!s->tx_fin_sent||final!=s->tx_block_id) goto protocol_error; s->tx_fin_acked=1; return NB_FEC_OK; }
    if(sscanf(msg,"FC:FIN:%u:%u",&sid,&final)==2){
        if(sid!=s->session_id||final<s->rx_next_block||(uint64_t)final-s->rx_next_block>s->cfg.rx_window_blocks) goto protocol_error;
        s->rx_fin_known=1; s->rx_final_exclusive=final; return deliver_ready(s);
    }
    if(sscanf(msg,"FC:RST:%u:%u",&sid,&code)==2){ if(sid!=s->session_id) goto protocol_error; s->failed=1; return NB_FEC_ERR_PROTOCOL; }
    {
        char* last=strrchr(msg,':');
        if(last!=NULL&&sscanf(msg,"FC:RETX:%u:%u:%u:%u:%u:%u:",&sid,&block,&idx,&count,&raw,&pay)==6){
            if(sid!=s->session_id||idx>=count||count>s->cfg.k||pay>s->cfg.symbol_size||raw>65535u) goto protocol_error;
            const char* hex=last+1; uint8_t payload[NB_FEC_SYMBOL_SIZE]; int got=hex_decode(payload,sizeof(payload),hex,strlen(hex));
            if(got!=(int)pay) goto protocol_error;
            uint8_t wire[NB_FEC_DGRAM_HEADER+NB_FEC_SYMBOL_SIZE];
            int n=build_datagram(wire,sizeof(wire),NB_FEC_DGRAM_SOURCE,sid,block,(uint8_t)idx,(uint8_t)count,(uint16_t)pay,(uint16_t)raw,payload);
            if(n<0) goto protocol_error;
            s->metrics.retx_received++;
            int rc=nb_fec_on_datagram(s,wire,(size_t)n,now_us);
            if(rc!=NB_FEC_OK) return rc;
            return send_controlf(s,"FC:RACK:%u:%u:%u\n",sid,block,idx);
        }
    }
protocol_error:
    s->metrics.protocol_errors++;
    return NB_FEC_ERR_PROTOCOL;
}

int nb_fec_tick(nb_fec_session_t* s, uint64_t now_us){
    if(s==NULL||s->failed) return NB_FEC_ERR_ARGUMENT;
    if(s->tx_raw_bytes>0&&s->tx_first_us>0&&now_us>=s->tx_first_us&&now_us-s->tx_first_us>=s->cfg.block_hold_us){
        int rc=flush_tx_block(s); if(rc!=NB_FEC_OK) return rc;
    }
    for(uint32_t i=0;i<s->cfg.rx_window_blocks;i++){
        nb_fec_rx_block_t* b=s->rx_window[i]; if(b==NULL||b->src_count==0) continue;
        uint32_t missing=0; for(uint8_t j=0;j<b->src_count;j++) if(!b->src_present[j]) missing|=(1u<<j);
        if(missing==0) continue;
        uint64_t due=b->nack_count==0?b->first_us+s->cfg.nack_delay_us:b->last_nack_us+s->cfg.nack_retry_us;
        if(now_us<due) continue;
        if(b->nack_count>=s->cfg.max_nack_retries){
            s->metrics.retry_exhausted++; s->failed=1; (void)nb_fec_send_reset(s,3u); return NB_FEC_ERR_RETRY_EXHAUSTED;
        }
        int rc=send_controlf(s,"FC:NACK:%u:%u:%u\n",s->session_id,b->block_id,missing); if(rc!=NB_FEC_OK) return rc;
        if(b->nack_count==0) s->metrics.nack_sent++; else s->metrics.nack_retried++;
        b->nack_count++; b->last_nack_us=now_us;
    }
    return deliver_ready(s);
}

int nb_fec_send_reset(nb_fec_session_t* s, uint32_t code){
    if(s==NULL) return NB_FEC_ERR_ARGUMENT;
    return send_controlf(s,"FC:RST:%u:%u\n",s->session_id,code);
}

uint64_t nb_fec_next_deadline(const nb_fec_session_t* s){
    if(s==NULL||s->failed) return 0;
    uint64_t next=s->tx_raw_bytes>0&&s->tx_first_us>0?s->tx_first_us+s->cfg.block_hold_us:0;
    for(uint32_t i=0;i<s->cfg.rx_window_blocks;i++){
        const nb_fec_rx_block_t* b=s->rx_window[i]; if(b==NULL||b->src_count==0) continue;
        uint32_t missing=0; for(uint8_t j=0;j<b->src_count;j++) if(!b->src_present[j]) missing|=(1u<<j);
        if(missing==0) continue;
        uint64_t due=b->nack_count==0?b->first_us+s->cfg.nack_delay_us:b->last_nack_us+s->cfg.nack_retry_us;
        if(next==0||due<next) next=due;
    }
    return next;
}

const nb_fec_metrics_t* nb_fec_metrics(const nb_fec_session_t* s){ return s==NULL?NULL:&s->metrics; }
int nb_fec_failed(const nb_fec_session_t* s){ return s==NULL?1:s->failed; }
