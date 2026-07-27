#include "nb_send.h"

int nb_send_append(nb_ring_t* ring,nb_live_queue_clock_t* clock,const uint8_t* data,size_t length,
    size_t limit,uint64_t now_us,size_t* peak){
    if(ring==NULL||clock==NULL)return -1;
    size_t previous=ring->len;
    if(nb_ring_append(ring,data,length,limit)!=0)return -1;
    nb_live_queue_appended(clock,previous,length,now_us);
    if(peak&&ring->len>*peak)*peak=ring->len;
    return 0;
}

size_t nb_send_copyout(nb_ring_t* ring,nb_live_queue_clock_t* clock,uint8_t* destination,size_t length){
    if(ring==NULL||clock==NULL)return 0;
    size_t copied=nb_ring_copyout(ring,destination,length);
    nb_live_queue_consumed(clock,copied,ring->len);return copied;
}

void nb_send_consume(nb_ring_t* ring,nb_live_queue_clock_t* clock,size_t length){
    if(ring==NULL||clock==NULL)return;
    nb_ring_consume(ring,length);nb_live_queue_consumed(clock,length,ring->len);
}

void nb_send_clear(nb_ring_t* ring,nb_live_queue_clock_t* clock){
    if(ring==NULL||clock==NULL)return;
    nb_ring_clear(ring);nb_live_queue_consumed(clock,0,0);
}

int nb_send_local_write_pending(size_t queued,int connecting,int fin_pending){
    return queued>0||connecting||fin_pending;
}
