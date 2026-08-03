#include "nb_ring.h"

#include <stdlib.h>
#include <string.h>

static int ring_reserve(nb_ring_t* q, size_t need, size_t limit){
    if(need>limit) return -1;
    if(q->cap>=need) return 0;
    size_t cap=q->cap?q->cap:4096u;
    while(cap<need){
        if(cap>limit/2u){cap=limit;break;}
        cap*=2u;
    }
    uint8_t* data=malloc(cap); if(data==NULL)return -1;
    if(q->len>0){
        size_t first=q->cap-q->head; if(first>q->len)first=q->len;
        memcpy(data,q->data+q->head,first);
        if(first<q->len)memcpy(data+first,q->data,q->len-first);
    }
    free(q->data);q->data=data;q->cap=cap;q->head=0;return 0;
}

void nb_ring_bind_budget(nb_ring_t* q,uint64_t* used,uint64_t limit){
    if(q==NULL)return;
    q->budget_used=used;q->budget_limit=limit;
}

static int budget_take(nb_ring_t* q,size_t len){
    if(q->budget_used==NULL||q->budget_limit==0)return 0;
    if(*q->budget_used>q->budget_limit||len>q->budget_limit-*q->budget_used)return -1;
    *q->budget_used+=(uint64_t)len;return 0;
}

static void budget_release(nb_ring_t* q,size_t len){
    if(q->budget_used==NULL)return;
    if((uint64_t)len>=*q->budget_used)*q->budget_used=0;
    else *q->budget_used-=(uint64_t)len;
}

void nb_ring_dispose(nb_ring_t* q){if(q){budget_release(q,q->len);free(q->data);memset(q,0,sizeof(*q));}}

int nb_ring_append(nb_ring_t* q,const uint8_t* data,size_t len,size_t limit){
    if(q==NULL||(len>0&&data==NULL)||q->len>limit||len>limit-q->len)return -1;
    if(len==0)return 0;
    if(budget_take(q,len)!=0)return -1;
    if(ring_reserve(q,q->len+len,limit)!=0){budget_release(q,len);return -1;}
    size_t tail=(q->head+q->len)%q->cap;
    size_t first=q->cap-tail;if(first>len)first=len;
    memcpy(q->data+tail,data,first);if(first<len)memcpy(q->data,data+first,len-first);
    q->len+=len;return 0;
}

size_t nb_ring_peek(const nb_ring_t* q,const uint8_t** data){
    if(data) *data=NULL;
    if(q==NULL||q->len==0) return 0;
    if(data) *data=q->data+q->head;
    size_t n=q->cap-q->head;return n<q->len?n:q->len;
}

void nb_ring_consume(nb_ring_t* q,size_t len){
    if(q==NULL||q->len==0)return;
    if(len>=q->len){budget_release(q,q->len);q->head=0;q->len=0;return;}
    budget_release(q,len);
    q->head=(q->head+len)%q->cap;q->len-=len;
}

size_t nb_ring_copyout(nb_ring_t* q,uint8_t* dst,size_t len){
    if(q==NULL||dst==NULL) return 0;
    if(len>q->len) len=q->len;
    size_t done=0;
    while(done<len){const uint8_t* p=NULL;size_t n=nb_ring_peek(q,&p);if(n>len-done)n=len-done;memcpy(dst+done,p,n);nb_ring_consume(q,n);done+=n;}
    return done;
}

void nb_ring_clear(nb_ring_t* q){if(q){budget_release(q,q->len);q->head=0;q->len=0;}}

size_t nb_ring_release_empty(nb_ring_t* q,size_t capacity_threshold){
    if(q==NULL||q->len!=0||q->cap<=capacity_threshold)return 0;
    size_t released=q->cap;uint64_t* used=q->budget_used;uint64_t limit=q->budget_limit;
    free(q->data);memset(q,0,sizeof(*q));q->budget_used=used;q->budget_limit=limit;return released;
}
