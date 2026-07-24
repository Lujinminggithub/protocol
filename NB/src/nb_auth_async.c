#define _POSIX_C_SOURCE 200809L
#include "nb_auth_async.h"

#include <fcntl.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NB_AUTH_ASYNC_QUEUE_CAP 1024

typedef struct {
    uint32_t session_id;
    char username[NB_AUTH_NAME_MAX];
    unsigned char password[256];
    size_t password_len;
    unsigned char fingerprint[NB_AUTH_FINGERPRINT_LEN];
} nb_auth_async_request_t;

static struct {
    nb_auth_async_request_t requests[NB_AUTH_ASYNC_QUEUE_CAP];
    nb_auth_async_result_t results[NB_AUTH_ASYNC_QUEUE_CAP];
    size_t request_head,request_tail,result_head,result_tail;
    pthread_mutex_t mutex;
    pthread_cond_t request_ready,result_space;
    const nb_auth_users_t* users;
    int pipe_read,pipe_write,started;
} state;

static size_t next_slot(size_t value){return (value+1)%NB_AUTH_ASYNC_QUEUE_CAP;}
static uint64_t monotonic_us(void){
    struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    return (uint64_t)ts.tv_sec*1000000ULL+(uint64_t)ts.tv_nsec/1000ULL;
}

static void* auth_worker(void* unused){
    (void)unused;
    for(;;){
        pthread_mutex_lock(&state.mutex);
        while(state.request_head==state.request_tail)pthread_cond_wait(&state.request_ready,&state.mutex);
        nb_auth_async_request_t request=state.requests[state.request_head];
        OPENSSL_cleanse(&state.requests[state.request_head],sizeof(state.requests[state.request_head]));
        state.request_head=next_slot(state.request_head);pthread_mutex_unlock(&state.mutex);

        uint64_t started=monotonic_us();
        nb_auth_async_result_t result;memset(&result,0,sizeof(result));
        result.session_id=request.session_id;
        memcpy(result.username,request.username,sizeof(result.username));
        memcpy(result.fingerprint,request.fingerprint,sizeof(result.fingerprint));
        result.valid=nb_auth_user_verify(state.users,request.username,request.password,request.password_len);
        uint64_t finished=monotonic_us();result.work_us=finished>=started?finished-started:0;
        OPENSSL_cleanse(&request,sizeof(request));

        pthread_mutex_lock(&state.mutex);size_t next=next_slot(state.result_tail);
        while(next==state.result_head){pthread_cond_wait(&state.result_space,&state.mutex);next=next_slot(state.result_tail);}
        state.results[state.result_tail]=result;state.result_tail=next;pthread_mutex_unlock(&state.mutex);
        ssize_t written=write(state.pipe_write,"a",1);(void)written;
    }
    return NULL;
}

int nb_auth_async_init(const nb_auth_users_t* users,int worker_count){
    if(state.started)return state.users==users?0:-1;
    if(users==NULL||users->count==0||worker_count<1||worker_count>32)return -1;
    int descriptors[2];if(pipe(descriptors)!=0)return -1;
    state.pipe_read=descriptors[0];state.pipe_write=descriptors[1];state.users=users;
    if(fcntl(state.pipe_read,F_SETFL,fcntl(state.pipe_read,F_GETFL,0)|O_NONBLOCK)!=0||
       fcntl(state.pipe_write,F_SETFL,fcntl(state.pipe_write,F_GETFL,0)|O_NONBLOCK)!=0)return -1;
    (void)fcntl(state.pipe_read,F_SETFD,FD_CLOEXEC);(void)fcntl(state.pipe_write,F_SETFD,FD_CLOEXEC);
    pthread_mutex_init(&state.mutex,NULL);pthread_cond_init(&state.request_ready,NULL);pthread_cond_init(&state.result_space,NULL);
    int created=0;for(int i=0;i<worker_count;i++){pthread_t thread;
        if(pthread_create(&thread,NULL,auth_worker,NULL)==0){pthread_detach(thread);created++;}}
    if(created==0)return -1;
    state.started=1;return 0;
}

int nb_auth_async_result_fd(void){return state.started?state.pipe_read:-1;}

int nb_auth_async_submit(uint32_t session_id,const char* username,
    const unsigned char* password,size_t password_len){
    if(!state.started||session_id==0||username==NULL||username[0]==0||
        strlen(username)>=NB_AUTH_NAME_MAX||password==NULL||password_len==0||password_len>255)return -1;
    nb_auth_async_request_t request;memset(&request,0,sizeof(request));request.session_id=session_id;
    memcpy(request.username,username,strlen(username)+1);memcpy(request.password,password,password_len);request.password_len=password_len;
    if(!nb_auth_fingerprint(state.users,username,password,password_len,request.fingerprint)){
        OPENSSL_cleanse(&request,sizeof(request));return -1;
    }
    pthread_mutex_lock(&state.mutex);size_t next=next_slot(state.request_tail);
    if(next==state.request_head){pthread_mutex_unlock(&state.mutex);OPENSSL_cleanse(&request,sizeof(request));return -1;}
    state.requests[state.request_tail]=request;state.request_tail=next;pthread_cond_signal(&state.request_ready);
    pthread_mutex_unlock(&state.mutex);OPENSSL_cleanse(&request,sizeof(request));return 0;
}

int nb_auth_async_pop(nb_auth_async_result_t* result){
    if(!state.started||result==NULL)return 0;
    pthread_mutex_lock(&state.mutex);
    if(state.result_head==state.result_tail){pthread_mutex_unlock(&state.mutex);return 0;}
    *result=state.results[state.result_head];memset(&state.results[state.result_head],0,sizeof(state.results[state.result_head]));
    state.result_head=next_slot(state.result_head);pthread_cond_signal(&state.result_space);
    pthread_mutex_unlock(&state.mutex);return 1;
}
