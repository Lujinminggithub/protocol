#define _POSIX_C_SOURCE 200112L
#include "nb_dns.h"

#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NB_DNS_QUEUE_CAP 4096
#define NB_DNS_MAX_WORKERS 32

typedef struct {uint32_t session_id;int port;char host[256];} nb_dns_request_t;
struct nb_dns {
    nb_dns_request_t requests[NB_DNS_QUEUE_CAP];int request_head,request_tail;
    nb_dns_result_t results[NB_DNS_QUEUE_CAP];int result_head,result_tail;
    pthread_mutex_t mutex;pthread_cond_t condition;
    pthread_t workers[NB_DNS_MAX_WORKERS];int worker_count;
    int pipe_read,pipe_write,address_family,stopping;
};

static _Thread_local nb_dns_t* legacy;
static int next_slot(int value){return (value+1)%NB_DNS_QUEUE_CAP;}

static void* worker(void* context){
    nb_dns_t* dns=context;
    for(;;){
        pthread_mutex_lock(&dns->mutex);
        while(!dns->stopping&&dns->request_head==dns->request_tail)pthread_cond_wait(&dns->condition,&dns->mutex);
        if(dns->stopping){pthread_mutex_unlock(&dns->mutex);break;}
        nb_dns_request_t request=dns->requests[dns->request_head];dns->request_head=next_slot(dns->request_head);
        pthread_mutex_unlock(&dns->mutex);
        struct addrinfo hints,*addresses=NULL;char port_text[16];memset(&hints,0,sizeof(hints));
        hints.ai_family=dns->address_family;hints.ai_socktype=SOCK_STREAM;snprintf(port_text,sizeof(port_text),"%d",request.port);
        nb_dns_result_t result;memset(&result,0,sizeof(result));result.ps_id=request.session_id;result.port=request.port;
        if(getaddrinfo(request.host,port_text,&hints,&addresses)==0&&addresses){
            if(addresses->ai_addrlen<=sizeof(result.addr)){
                memcpy(&result.addr,addresses->ai_addr,addresses->ai_addrlen);
                result.addrlen=addresses->ai_addrlen;result.ok=1;
            }
            freeaddrinfo(addresses);
        }
        pthread_mutex_lock(&dns->mutex);int next=next_slot(dns->result_tail);
        if(next!=dns->result_head){dns->results[dns->result_tail]=result;dns->result_tail=next;}
        pthread_mutex_unlock(&dns->mutex);ssize_t written=write(dns->pipe_write,"x",1);(void)written;
    }
    return NULL;
}

nb_dns_t* nb_dns_create(int family,int worker_count){
    if(worker_count<1||worker_count>NB_DNS_MAX_WORKERS)return NULL;
    nb_dns_t* dns=calloc(1,sizeof(*dns));if(dns==NULL)return NULL;
    dns->pipe_read=-1;dns->pipe_write=-1;dns->address_family=family;
    int descriptors[2];if(pipe(descriptors)!=0){free(dns);return NULL;}
    dns->pipe_read=descriptors[0];dns->pipe_write=descriptors[1];
    if(fcntl(dns->pipe_read,F_SETFL,fcntl(dns->pipe_read,F_GETFL,0)|O_NONBLOCK)!=0||
       fcntl(dns->pipe_write,F_SETFL,fcntl(dns->pipe_write,F_GETFL,0)|O_NONBLOCK)!=0){nb_dns_destroy(dns);return NULL;}
    pthread_mutex_init(&dns->mutex,NULL);pthread_cond_init(&dns->condition,NULL);
    for(int i=0;i<worker_count;i++){
        if(pthread_create(&dns->workers[dns->worker_count],NULL,worker,dns)==0)dns->worker_count++;
    }
    if(dns->worker_count==0){nb_dns_destroy(dns);return NULL;}
    return dns;
}

void nb_dns_destroy(nb_dns_t* dns){
    if(dns==NULL)return;
    if(dns->worker_count>0){pthread_mutex_lock(&dns->mutex);dns->stopping=1;pthread_cond_broadcast(&dns->condition);pthread_mutex_unlock(&dns->mutex);}
    for(int i=0;i<dns->worker_count;i++)pthread_join(dns->workers[i],NULL);
    if(dns->worker_count>0){pthread_cond_destroy(&dns->condition);pthread_mutex_destroy(&dns->mutex);}
    if(dns->pipe_read>=0)close(dns->pipe_read);
    if(dns->pipe_write>=0)close(dns->pipe_write);
    free(dns);
}

int nb_dns_context_result_fd(nb_dns_t* dns){return dns?dns->pipe_read:-1;}

int nb_dns_context_submit(nb_dns_t* dns,uint32_t session_id,const char* host,int port){
    if(dns==NULL||session_id==0||host==NULL||host[0]==0||strlen(host)>=sizeof(dns->requests[0].host)||port<=0||port>65535)return -1;
    pthread_mutex_lock(&dns->mutex);int next=next_slot(dns->request_tail);
    if(next==dns->request_head){pthread_mutex_unlock(&dns->mutex);return -1;}
    nb_dns_request_t* request=&dns->requests[dns->request_tail];request->session_id=session_id;request->port=port;
    memcpy(request->host,host,strlen(host)+1);dns->request_tail=next;pthread_cond_signal(&dns->condition);
    pthread_mutex_unlock(&dns->mutex);return 0;
}

int nb_dns_context_pop(nb_dns_t* dns,nb_dns_result_t* result){
    if(dns==NULL||result==NULL)return 0;
    pthread_mutex_lock(&dns->mutex);
    if(dns->result_head==dns->result_tail){pthread_mutex_unlock(&dns->mutex);return 0;}
    *result=dns->results[dns->result_head];dns->result_head=next_slot(dns->result_head);
    pthread_mutex_unlock(&dns->mutex);return 1;
}

int nb_dns_init(int family,int worker_count){if(legacy)return 0;legacy=nb_dns_create(family,worker_count);return legacy?0:-1;}
int nb_dns_result_fd(void){return nb_dns_context_result_fd(legacy);}
int nb_dns_submit(uint32_t session_id,const char* host,int port){return nb_dns_context_submit(legacy,session_id,host,port);}
int nb_dns_pop(nb_dns_result_t* result){return nb_dns_context_pop(legacy,result);}
