#define _POSIX_C_SOURCE 200112L
#include "nb_dns.h"

#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NB_DNS_QUEUE_CAP 4096

typedef struct {uint32_t session_id;int port;char host[256];} nb_dns_request_t;
static struct {
    nb_dns_request_t requests[NB_DNS_QUEUE_CAP];int request_head,request_tail;
    nb_dns_result_t results[NB_DNS_QUEUE_CAP];int result_head,result_tail;
    pthread_mutex_t mutex;pthread_cond_t condition;
    int pipe_read,pipe_write,address_family,started;
} state;

static int next_slot(int value){return (value+1)%NB_DNS_QUEUE_CAP;}

static void* worker(void* unused){
    (void)unused;
    for(;;){
        pthread_mutex_lock(&state.mutex);
        while(state.request_head==state.request_tail)pthread_cond_wait(&state.condition,&state.mutex);
        nb_dns_request_t request=state.requests[state.request_head];state.request_head=next_slot(state.request_head);
        pthread_mutex_unlock(&state.mutex);
        struct addrinfo hints,*addresses=NULL;char port_text[16];memset(&hints,0,sizeof(hints));
        hints.ai_family=state.address_family;hints.ai_socktype=SOCK_STREAM;snprintf(port_text,sizeof(port_text),"%d",request.port);
        nb_dns_result_t result;memset(&result,0,sizeof(result));result.ps_id=request.session_id;result.port=request.port;
        if(getaddrinfo(request.host,port_text,&hints,&addresses)==0&&addresses){
            if(addresses->ai_addrlen<=sizeof(result.addr)){
                memcpy(&result.addr,addresses->ai_addr,addresses->ai_addrlen);
                result.addrlen=addresses->ai_addrlen;result.ok=1;
            }
            freeaddrinfo(addresses);
        }
        pthread_mutex_lock(&state.mutex);int next=next_slot(state.result_tail);
        if(next!=state.result_head){state.results[state.result_tail]=result;state.result_tail=next;}
        pthread_mutex_unlock(&state.mutex);ssize_t written=write(state.pipe_write,"x",1);(void)written;
    }
    return NULL;
}

int nb_dns_init(int family,int worker_count){
    if(state.started)return 0;
    if(worker_count<1||worker_count>32)return -1;
    int descriptors[2];if(pipe(descriptors)!=0)return -1;state.pipe_read=descriptors[0];state.pipe_write=descriptors[1];
    state.address_family=family;
    if(fcntl(state.pipe_read,F_SETFL,fcntl(state.pipe_read,F_GETFL,0)|O_NONBLOCK)!=0||
       fcntl(state.pipe_write,F_SETFL,fcntl(state.pipe_write,F_GETFL,0)|O_NONBLOCK)!=0)return -1;
    pthread_mutex_init(&state.mutex,NULL);pthread_cond_init(&state.condition,NULL);
    int created=0;for(int i=0;i<worker_count;i++){pthread_t thread;if(pthread_create(&thread,NULL,worker,NULL)==0){pthread_detach(thread);created++;}}
    if(created==0)return -1;
    state.started=1;return 0;
}

int nb_dns_result_fd(void){return state.started?state.pipe_read:-1;}

int nb_dns_submit(uint32_t session_id,const char* host,int port){
    if(!state.started||session_id==0||host==NULL||host[0]==0||strlen(host)>=sizeof(state.requests[0].host)||port<=0||port>65535)return -1;
    pthread_mutex_lock(&state.mutex);int next=next_slot(state.request_tail);
    if(next==state.request_head){pthread_mutex_unlock(&state.mutex);return -1;}
    nb_dns_request_t* request=&state.requests[state.request_tail];request->session_id=session_id;request->port=port;
    memcpy(request->host,host,strlen(host)+1);state.request_tail=next;pthread_cond_signal(&state.condition);
    pthread_mutex_unlock(&state.mutex);return 0;
}

int nb_dns_pop(nb_dns_result_t* result){
    if(!state.started||result==NULL)return 0;
    pthread_mutex_lock(&state.mutex);
    if(state.result_head==state.result_tail){pthread_mutex_unlock(&state.mutex);return 0;}
    *result=state.results[state.result_head];state.result_head=next_slot(state.result_head);
    pthread_mutex_unlock(&state.mutex);return 1;
}
