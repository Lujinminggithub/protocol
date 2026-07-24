#include "nb_udp_lifecycle.h"

#include <string.h>

void nb_udp_lifecycle_init(nb_udp_lifecycle_t* state){if(state)memset(state,0,sizeof(*state));}

static nb_udp_lifecycle_item_t* find_item(nb_udp_lifecycle_t* state,uintptr_t link,
    uint8_t type,uint32_t session_id){
    if(!state)return NULL;
    for(size_t i=0;i<NB_UDP_LIFECYCLE_CAP;i++){
        nb_udp_lifecycle_item_t* item=&state->items[i];
        if(item->active&&item->link==link&&item->type==type&&item->session_id==session_id)return item;
    }
    return NULL;
}

int nb_udp_lifecycle_schedule(nb_udp_lifecycle_t* state,uintptr_t link,uint8_t type,
    uint32_t session_id,const char* route,uint64_t now_us){
    if(!state||link==0||session_id==0||!route||!*route||
        (type!=NB_UDP_TYPE_CLOSE&&type!=NB_UDP_TYPE_CLOSE_ACK))return -1;
    nb_udp_lifecycle_item_t* item=find_item(state,link,type,session_id);
    if(item){item->next_send_at=now_us;item->ready_marked=0;return 0;}
    for(size_t i=0;i<NB_UDP_LIFECYCLE_CAP;i++)if(!state->items[i].active){
        item=&state->items[i];memset(item,0,sizeof(*item));uint8_t payload=0;
        int n=nb_udp_wire_encode(item->wire,sizeof(item->wire),type,session_id,0,0,1,1,
            route,(uint16_t)strlen(route),&payload,1);
        if(n<0)return -1;
        item->link=link;item->session_id=session_id;item->next_send_at=now_us;
        item->wire_length=(uint16_t)n;item->type=type;item->active=1;return 0;
    }
    return -1;
}

int nb_udp_lifecycle_next(nb_udp_lifecycle_t* state,uintptr_t link,uint64_t now_us,
    const uint8_t** wire,size_t* wire_length,size_t* token){
    if(!state||!wire||!wire_length||!token)return 0;
    for(size_t i=0;i<NB_UDP_LIFECYCLE_CAP;i++){
        nb_udp_lifecycle_item_t* item=&state->items[i];
        if(item->active&&item->link==link&&item->next_send_at<=now_us){
            *wire=item->wire;*wire_length=item->wire_length;*token=i;return 1;
        }
    }
    return 0;
}

void nb_udp_lifecycle_sent(nb_udp_lifecycle_t* state,size_t token,uint64_t now_us){
    if(!state||token>=NB_UDP_LIFECYCLE_CAP||!state->items[token].active)return;
    nb_udp_lifecycle_item_t* item=&state->items[token];item->ready_marked=0;item->attempts++;
    if(item->type==NB_UDP_TYPE_CLOSE_ACK||item->attempts>=NB_UDP_CLOSE_ATTEMPTS){
        memset(item,0,sizeof(*item));return;
    }
    item->next_send_at=now_us+NB_UDP_CLOSE_RETRY_US;
}

int nb_udp_lifecycle_ack(nb_udp_lifecycle_t* state,uintptr_t link,uint32_t session_id){
    nb_udp_lifecycle_item_t* item=find_item(state,link,NB_UDP_TYPE_CLOSE,session_id);
    if(!item)return 0;
    memset(item,0,sizeof(*item));return 1;
}

uintptr_t nb_udp_lifecycle_mark_due(nb_udp_lifecycle_t* state,uint64_t now_us){
    if(!state)return 0;
    for(size_t i=0;i<NB_UDP_LIFECYCLE_CAP;i++){
        nb_udp_lifecycle_item_t* item=&state->items[i];
        if(item->active&&!item->ready_marked&&item->next_send_at<=now_us){item->ready_marked=1;return item->link;}
    }
    return 0;
}

uint64_t nb_udp_lifecycle_next_deadline(const nb_udp_lifecycle_t* state){
    uint64_t deadline=0;if(!state)return 0;
    for(size_t i=0;i<NB_UDP_LIFECYCLE_CAP;i++)if(state->items[i].active&&
        (deadline==0||state->items[i].next_send_at<deadline))deadline=state->items[i].next_send_at;
    return deadline;
}

void nb_udp_lifecycle_forget_link(nb_udp_lifecycle_t* state,uintptr_t link){
    if(!state||link==0)return;
    for(size_t i=0;i<NB_UDP_LIFECYCLE_CAP;i++)if(state->items[i].active&&state->items[i].link==link)
        memset(&state->items[i],0,sizeof(state->items[i]));
}

size_t nb_udp_lifecycle_count(const nb_udp_lifecycle_t* state){
    size_t count=0;if(!state)return 0;
    for(size_t i=0;i<NB_UDP_LIFECYCLE_CAP;i++)count+=state->items[i].active!=0;
    return count;
}
