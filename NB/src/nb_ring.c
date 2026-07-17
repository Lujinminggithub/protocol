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

void nb_ring_dispose(nb_ring_t* q){if(q){free(q->data);memset(q,0,sizeof(*q));}}

int nb_ring_append(nb_ring_t* q,const uint8_t* data,size_t len,size_t limit){
    if(q==NULL||(len>0&&data==NULL)||q->len>limit||len>limit-q->len)return -1;
    if(len==0)return 0;
    if(ring_reserve(q,q->len+len,limit)!=0)return -1;
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
    if(len>=q->len){q->head=0;q->len=0;return;}
    q->head=(q->head+len)%q->cap;q->len-=len;
}

size_t nb_ring_copyout(nb_ring_t* q,uint8_t* dst,size_t len){
    if(q==NULL||dst==NULL) return 0;
    if(len>q->len) len=q->len;
    size_t done=0;
    while(done<len){const uint8_t* p=NULL;size_t n=nb_ring_peek(q,&p);if(n>len-done)n=len-done;memcpy(dst+done,p,n);nb_ring_consume(q,n);done+=n;}
    return done;
}

void nb_ring_clear(nb_ring_t* q){if(q){q->head=0;q->len=0;}}
