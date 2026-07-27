#include "../src/nb_fec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct packet {
    uint8_t* data;
    size_t len;
    struct packet* next;
} packet_t;

typedef struct {
    packet_t* dg_head;
    packet_t* dg_tail;
    packet_t* ctl_head;
    packet_t* ctl_tail;
    uint8_t* delivered;
    size_t delivered_len;
    size_t delivered_cap;
    int fin;
} endpoint_t;

static int queue_push(packet_t** head, packet_t** tail, const uint8_t* data, size_t len){
    packet_t* p=calloc(1,sizeof(*p));
    if(p==NULL) return -1;
    p->data=malloc(len?len:1); if(p->data==NULL){ free(p); return -1; }
    if(len) memcpy(p->data,data,len);
    p->len=len;
    if(*tail) (*tail)->next=p; else *head=p; *tail=p;
    return 0;
}
static packet_t* queue_pop(packet_t** head, packet_t** tail){
    packet_t* p=*head; if(p==NULL) return NULL; *head=p->next; if(*head==NULL)*tail=NULL; p->next=NULL; return p;
}
static int cb_dg(void* ctx,const uint8_t* d,size_t n){ endpoint_t* e=ctx; return queue_push(&e->dg_head,&e->dg_tail,d,n); }
static int cb_ctl(void* ctx,const uint8_t* d,size_t n){ endpoint_t* e=ctx; return queue_push(&e->ctl_head,&e->ctl_tail,d,n); }
static int cb_deliver(void* ctx,const uint8_t* d,size_t n,int fin){
    endpoint_t* e=ctx;
    if(e->delivered_len+n>e->delivered_cap){
        size_t cap=e->delivered_cap?e->delivered_cap*2:4096; while(cap<e->delivered_len+n)cap*=2;
        uint8_t* p=realloc(e->delivered,cap); if(p==NULL)return -1; e->delivered=p; e->delivered_cap=cap;
    }
    if(n)memcpy(e->delivered+e->delivered_len,d,n);
    e->delivered_len+=n;
    if(fin)e->fin=1;
    return 0;
}
static void packet_free(packet_t* p){ if(p){free(p->data);free(p);} }
static void endpoint_free(endpoint_t* e){
    packet_t* p; while((p=queue_pop(&e->dg_head,&e->dg_tail))!=NULL)packet_free(p);
    while((p=queue_pop(&e->ctl_head,&e->ctl_tail))!=NULL)packet_free(p);
    free(e->delivered);
}
static int drain_ctl(endpoint_t* from,nb_fec_session_t* to,uint64_t now){
    packet_t* p; while((p=queue_pop(&from->ctl_head,&from->ctl_tail))!=NULL){
        int rc=nb_fec_on_control(to,(const char*)p->data,p->len,now); packet_free(p); if(rc!=NB_FEC_OK)return rc;
    } return NB_FEC_OK;
}
static size_t list_count(packet_t* p){size_t n=0;for(;p;p=p->next)n++;return n;}
static packet_t** list_array(packet_t** head,packet_t** tail,size_t* count){
    *count=list_count(*head); packet_t** a=calloc(*count,sizeof(*a)); if(a==NULL)return NULL;
    for(size_t i=0;i<*count;i++)a[i]=queue_pop(head,tail);
    return a;
}
static void fill_data(uint8_t* p,size_t n){uint32_t x=0x12345678u;for(size_t i=0;i<n;i++){x=x*1664525u+1013904223u;p[i]=(uint8_t)(x>>24);}}
static nb_fec_session_t* make_session(endpoint_t* e,const nb_fec_config_t* cfg){
    nb_fec_callbacks_t cb={cb_dg,cb_ctl,cb_deliver}; return nb_fec_session_create(77,cfg,&cb,e);
}
static int expect_payload(const char* name,endpoint_t* e,const uint8_t* data,size_t len){
    if(!e->fin||e->delivered_len!=len||memcmp(e->delivered,data,len)!=0){
        fprintf(stderr,"FAIL %s fin=%d got=%zu expected=%zu\n",name,e->fin,e->delivered_len,len);return -1;
    } printf("PASS %s bytes=%zu\n",name,len);return 0;
}

static int test_out_of_order(void){
    nb_fec_config_t cfg; if(nb_fec_config_for_bdp(&cfg,4,2,20000000,200000,2)!=0)return -1;
    if(cfg.tx_history_blocks<=8||cfg.rx_window_blocks<=8||cfg.nack_delay_us!=200000||
        cfg.nack_retry_us!=400000||cfg.max_nack_retries!=20){fprintf(stderr,"FAIL bdp sizing\n");return -1;}
    endpoint_t a={0},b={0}; nb_fec_session_t* tx=make_session(&a,&cfg),*rx=make_session(&b,&cfg); if(!tx||!rx)return -1;
    size_t len=NB_FEC_SYMBOL_SIZE*19u+137u; uint8_t* data=malloc(len);fill_data(data,len);
    if(nb_fec_tx_feed(tx,data,len,1,1000)!=0||drain_ctl(&a,rx,2000)!=0)return -1;
    size_t n=0;packet_t** arr=list_array(&a.dg_head,&a.dg_tail,&n);if(arr==NULL)return -1;
    for(size_t i=n;i>0;i--){int rc=nb_fec_on_datagram(rx,arr[i-1]->data,arr[i-1]->len,3000+i);packet_free(arr[i-1]);if(rc!=0){free(arr);return -1;}}
    free(arr); if(drain_ctl(&b,tx,5000)!=0)return -1;
    int rc=expect_payload("out-of-order",&b,data,len);
    const nb_fec_metrics_t* m=nb_fec_metrics(rx); if(!m||m->out_of_order_packets==0)rc=-1;
    free(data);nb_fec_session_destroy(tx);nb_fec_session_destroy(rx);endpoint_free(&a);endpoint_free(&b);return rc;
}

static int test_rs_recovery(void){
    nb_fec_config_t cfg;nb_fec_config_for_bdp(&cfg,4,2,5000000,200000,2);
    endpoint_t a={0},b={0};nb_fec_session_t* tx=make_session(&a,&cfg),*rx=make_session(&b,&cfg);
    size_t len=NB_FEC_SYMBOL_SIZE*8u;uint8_t* data=malloc(len);fill_data(data,len);
    if(nb_fec_tx_feed(tx,data,len,1,1000)!=0||drain_ctl(&a,rx,2000)!=0)return -1;
    packet_t* p;while((p=queue_pop(&a.dg_head,&a.dg_tail))!=NULL){
        uint8_t type=p->data[5],idx=p->data[14];
        if(!(type==3&&idx==0)){if(nb_fec_on_datagram(rx,p->data,p->len,3000)!=0){packet_free(p);return -1;}}packet_free(p);
    }
    int rc=expect_payload("rs-recovery",&b,data,len);const nb_fec_metrics_t*m=nb_fec_metrics(rx);
    if(!m||m->blocks_recovered!=2||m->recovered_source_bytes!=NB_FEC_SYMBOL_SIZE*2u)rc=-1;
    free(data);nb_fec_session_destroy(tx);nb_fec_session_destroy(rx);endpoint_free(&a);endpoint_free(&b);return rc;
}

static int test_nack_retx_and_exact_fin(void){
    nb_fec_config_t cfg;nb_fec_config_for_bdp(&cfg,4,2,5000000,200000,2);cfg.nack_delay_us=10000;cfg.nack_retry_us=20000;
    endpoint_t a={0},b={0};nb_fec_session_t* tx=make_session(&a,&cfg),*rx=make_session(&b,&cfg);
    size_t len=NB_FEC_SYMBOL_SIZE*4u;uint8_t* data=malloc(len);fill_data(data,len);
    if(nb_fec_tx_feed(tx,data,len,1,1000)!=0||drain_ctl(&a,rx,2000)!=0)return -1;
    packet_t*p;while((p=queue_pop(&a.dg_head,&a.dg_tail))!=NULL){
        if(p->data[5]==3&&p->data[14]==3){if(nb_fec_on_datagram(rx,p->data,p->len,3000)!=0){packet_free(p);return -1;}}packet_free(p);
    }
    if(nb_fec_tick(rx,2000+cfg.nack_delay_us)!=0)return -1;
    /* Drop first NACK, then verify retry. */
    packet_free(queue_pop(&b.ctl_head,&b.ctl_tail));
    if(nb_fec_tick(rx,36000)!=0||drain_ctl(&b,tx,37000)!=0||drain_ctl(&a,rx,38000)!=0||drain_ctl(&b,tx,39000)!=0)return -1;
    int rc=expect_payload("nack-retx-exact-fin",&b,data,len);const nb_fec_metrics_t*rm=nb_fec_metrics(rx);const nb_fec_metrics_t*tm=nb_fec_metrics(tx);
    if(!rm||!tm||rm->nack_retried==0||rm->retx_received!=3||tm->retx_acked!=3||!tm->block_acked)rc=-1;
    free(data);nb_fec_session_destroy(tx);nb_fec_session_destroy(rx);endpoint_free(&a);endpoint_free(&b);return rc;
}

static int test_strict_bounds(void){
    nb_fec_config_t cfg;nb_fec_config_for_bdp(&cfg,4,2,5000000,200000,2);endpoint_t e={0};nb_fec_session_t*s=make_session(&e,&cfg);if(!s)return -1;
    uint8_t bad[32]={0x4e,0x42,0x46,0x44,2,3};bad[9]=77;bad[15]=9;bad[16]=0;bad[17]=1;bad[18]=0;bad[19]=1;
    int rc=nb_fec_on_datagram(s,bad,21,1000);nb_fec_session_destroy(s);endpoint_free(&e);
    if(rc!=NB_FEC_ERR_PROTOCOL){fprintf(stderr,"FAIL strict-bounds rc=%d\n",rc);return -1;}printf("PASS strict-bounds\n");return 0;
}

static int test_windowed_large_payload(void){
    nb_fec_config_t cfg;nb_fec_config_for_bdp(&cfg,4,2,5000000,200000,2);
    cfg.tx_history_blocks=16;cfg.rx_window_blocks=48;
    endpoint_t a={0},b={0};nb_fec_session_t*tx=make_session(&a,&cfg),*rx=make_session(&b,&cfg);if(!tx||!rx)return -1;
    size_t len=1024u*1024u+137u,off=0;uint8_t*data=malloc(len);if(!data)return -1;fill_data(data,len);
    while(off<len){
        size_t writable=nb_fec_tx_writable(tx);if(writable==0){fprintf(stderr,"FAIL windowed-large stalled at %zu\n",off);return -1;}
        size_t take=len-off<writable?len-off:writable;int fin=off+take==len;
        if(nb_fec_tx_feed(tx,data+off,take,fin,1000+off)!=0)return -1;
        off+=take;
        if(drain_ctl(&a,rx,2000+off)!=0)return -1;
        packet_t*p;while((p=queue_pop(&a.dg_head,&a.dg_tail))!=NULL){int rc=nb_fec_on_datagram(rx,p->data,p->len,3000+off);packet_free(p);if(rc!=0)return -1;}
        if(drain_ctl(&b,tx,4000+off)!=0)return -1;
    }
    int rc=expect_payload("windowed-large",&b,data,len);
    const nb_fec_metrics_t*tm=nb_fec_metrics(tx);
    if(!tm||tm->window_errors!=0||tm->block_acked==0)rc=-1;
    free(data);nb_fec_session_destroy(tx);nb_fec_session_destroy(rx);endpoint_free(&a);endpoint_free(&b);return rc;
}

static int test_nack_head_of_line_only(void){
    nb_fec_config_t cfg;nb_fec_config_for_bdp(&cfg,4,2,5000000,200000,2);
    endpoint_t a={0},b={0};nb_fec_session_t*tx=make_session(&a,&cfg),*rx=make_session(&b,&cfg);if(!tx||!rx)return -1;
    size_t len=NB_FEC_SYMBOL_SIZE*8u;uint8_t*data=malloc(len);if(!data)return -1;fill_data(data,len);
    if(nb_fec_tx_feed(tx,data,len,1,1000)!=0||drain_ctl(&a,rx,2000)!=0)return -1;
    packet_t*p;while((p=queue_pop(&a.dg_head,&a.dg_tail))!=NULL)packet_free(p);
    if(nb_fec_tick(rx,2000+cfg.nack_delay_us)!=0)return -1;
    int rc=0;packet_t*nack=queue_pop(&b.ctl_head,&b.ctl_tail);
    if(nack==NULL||list_count(b.ctl_head)!=0||nack->len<13||memcmp(nack->data,"FC:NACK:77:0",12)!=0){
        fprintf(stderr,"FAIL nack-head-of-line\n");rc=-1;
    }else printf("PASS nack-head-of-line\n");
    packet_free(nack);free(data);nb_fec_session_destroy(tx);nb_fec_session_destroy(rx);endpoint_free(&a);endpoint_free(&b);return rc;
}

static int test_partial_block_deadline(void){
    nb_fec_config_t cfg;nb_fec_config_for_bdp(&cfg,4,2,5000000,200000,2);
    endpoint_t e={0};nb_fec_session_t*s=make_session(&e,&cfg);if(!s)return -1;
    uint8_t data[100];fill_data(data,sizeof(data));uint64_t started=10000;
    if(cfg.block_hold_us!=NB_FEC_DEFAULT_BLOCK_HOLD_US||nb_fec_tx_feed(s,data,sizeof(data),0,started)!=0)return -1;
    uint64_t deadline=started+NB_FEC_DEFAULT_BLOCK_HOLD_US;
    int rc=0;
    if(nb_fec_next_deadline(s)!=deadline||e.dg_head!=NULL||e.ctl_head!=NULL)rc=-1;
    if(nb_fec_tick(s,deadline-1)!=0||e.dg_head!=NULL||e.ctl_head!=NULL)rc=-1;
    if(nb_fec_tick(s,deadline)!=0||list_count(e.dg_head)!=3||list_count(e.ctl_head)!=1)rc=-1;
    if(rc)fprintf(stderr,"FAIL partial-block-deadline\n");else printf("PASS partial-block-deadline\n");
    nb_fec_session_destroy(s);endpoint_free(&e);return rc;
}

static int test_tx_complete_waits_for_finack(void){
    nb_fec_config_t cfg;nb_fec_config_for_bdp(&cfg,4,2,5000000,200000,2);
    endpoint_t a={0},b={0};nb_fec_session_t*tx=make_session(&a,&cfg),*rx=make_session(&b,&cfg);if(!tx||!rx)return -1;
    uint8_t data[100];fill_data(data,sizeof(data));int rc=0;
    if(nb_fec_tx_feed(tx,data,sizeof(data),1,1000)!=0||nb_fec_tx_complete(tx))rc=-1;
    if(drain_ctl(&a,rx,2000)!=0)rc=-1;
    packet_t*p;while((p=queue_pop(&a.dg_head,&a.dg_tail))!=NULL){
        if(nb_fec_on_datagram(rx,p->data,p->len,3000)!=0)rc=-1;
        packet_free(p);
    }
    if(nb_fec_tx_complete(tx)||drain_ctl(&b,tx,4000)!=0||!nb_fec_tx_complete(tx))rc=-1;
    if(rc)fprintf(stderr,"FAIL tx-complete-finack\n");else printf("PASS tx-complete-finack\n");
    nb_fec_session_destroy(tx);nb_fec_session_destroy(rx);endpoint_free(&a);endpoint_free(&b);return rc;
}

int main(void){
    int fail=0;fail|=test_out_of_order();fail|=test_rs_recovery();fail|=test_nack_retx_and_exact_fin();fail|=test_strict_bounds();fail|=test_windowed_large_payload();fail|=test_nack_head_of_line_only();fail|=test_partial_block_deadline();fail|=test_tx_complete_waits_for_finack();
    printf("RESULT %s\n",fail?"FAIL":"PASS");return fail?1:0;
}
