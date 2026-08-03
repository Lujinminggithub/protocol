#define _POSIX_C_SOURCE 200809L
#include "nb_auth_async.h"

#include <fcntl.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NB_AUTH_ASYNC_QUEUE_CAP 1024
#define NB_AUTH_ASYNC_MAX_WORKERS 32

typedef struct {
    uint32_t session_id;char username[NB_AUTH_NAME_MAX];unsigned char password[256];
    size_t password_len;unsigned char fingerprint[NB_AUTH_FINGERPRINT_LEN];
} nb_auth_async_request_t;

struct nb_auth_async {
    nb_auth_async_request_t requests[NB_AUTH_ASYNC_QUEUE_CAP];
    nb_auth_async_result_t results[NB_AUTH_ASYNC_QUEUE_CAP];
    size_t request_head,request_tail,result_head,result_tail;
    pthread_mutex_t mutex;pthread_cond_t request_ready,result_space;
    pthread_t workers[NB_AUTH_ASYNC_MAX_WORKERS];int worker_count;
    const nb_auth_users_t* users;int pipe_read,pipe_write,stopping;
};

static _Thread_local nb_auth_async_t* legacy;
static size_t next_slot(size_t value){return (value+1)%NB_AUTH_ASYNC_QUEUE_CAP;}
static uint64_t monotonic_us(void){
    struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    return (uint64_t)ts.tv_sec*1000000ULL+(uint64_t)ts.tv_nsec/1000ULL;
}

static void* auth_worker(void* context){
    nb_auth_async_t* auth=context;
    for(;;){
        pthread_mutex_lock(&auth->mutex);
        while(!auth->stopping&&auth->request_head==auth->request_tail)pthread_cond_wait(&auth->request_ready,&auth->mutex);
        if(auth->stopping){pthread_mutex_unlock(&auth->mutex);break;}
        nb_auth_async_request_t request=auth->requests[auth->request_head];
        OPENSSL_cleanse(&auth->requests[auth->request_head],sizeof(auth->requests[auth->request_head]));
        auth->request_head=next_slot(auth->request_head);pthread_mutex_unlock(&auth->mutex);
        uint64_t started=monotonic_us();nb_auth_async_result_t result;memset(&result,0,sizeof(result));
        result.session_id=request.session_id;memcpy(result.username,request.username,sizeof(result.username));
        memcpy(result.fingerprint,request.fingerprint,sizeof(result.fingerprint));
        result.valid=nb_auth_user_verify(auth->users,request.username,request.password,request.password_len);
        uint64_t finished=monotonic_us();result.work_us=finished>=started?finished-started:0;OPENSSL_cleanse(&request,sizeof(request));
        pthread_mutex_lock(&auth->mutex);size_t next=next_slot(auth->result_tail);
        while(!auth->stopping&&next==auth->result_head){pthread_cond_wait(&auth->result_space,&auth->mutex);next=next_slot(auth->result_tail);}
        if(!auth->stopping){auth->results[auth->result_tail]=result;auth->result_tail=next;}
        pthread_mutex_unlock(&auth->mutex);if(!auth->stopping){ssize_t written=write(auth->pipe_write,"a",1);(void)written;}
    }
    return NULL;
}

nb_auth_async_t* nb_auth_async_create(const nb_auth_users_t* users,int worker_count){
    if(users==NULL||users->count==0||worker_count<1||worker_count>NB_AUTH_ASYNC_MAX_WORKERS)return NULL;
    nb_auth_async_t* auth=calloc(1,sizeof(*auth));if(auth==NULL)return NULL;
    auth->pipe_read=-1;auth->pipe_write=-1;auth->users=users;int descriptors[2];
    if(pipe(descriptors)!=0){free(auth);return NULL;}auth->pipe_read=descriptors[0];auth->pipe_write=descriptors[1];
    if(fcntl(auth->pipe_read,F_SETFL,fcntl(auth->pipe_read,F_GETFL,0)|O_NONBLOCK)!=0||
       fcntl(auth->pipe_write,F_SETFL,fcntl(auth->pipe_write,F_GETFL,0)|O_NONBLOCK)!=0){nb_auth_async_destroy(auth);return NULL;}
    (void)fcntl(auth->pipe_read,F_SETFD,FD_CLOEXEC);(void)fcntl(auth->pipe_write,F_SETFD,FD_CLOEXEC);
    pthread_mutex_init(&auth->mutex,NULL);pthread_cond_init(&auth->request_ready,NULL);pthread_cond_init(&auth->result_space,NULL);
    for(int i=0;i<worker_count;i++)if(pthread_create(&auth->workers[auth->worker_count],NULL,auth_worker,auth)==0)auth->worker_count++;
    if(auth->worker_count==0){nb_auth_async_destroy(auth);return NULL;}return auth;
}

void nb_auth_async_destroy(nb_auth_async_t* auth){
    if(auth==NULL)return;
    if(auth->worker_count>0){pthread_mutex_lock(&auth->mutex);auth->stopping=1;pthread_cond_broadcast(&auth->request_ready);pthread_cond_broadcast(&auth->result_space);pthread_mutex_unlock(&auth->mutex);}
    for(int i=0;i<auth->worker_count;i++)pthread_join(auth->workers[i],NULL);
    if(auth->worker_count>0){pthread_cond_destroy(&auth->request_ready);pthread_cond_destroy(&auth->result_space);pthread_mutex_destroy(&auth->mutex);}
    if(auth->pipe_read>=0)close(auth->pipe_read);
    if(auth->pipe_write>=0)close(auth->pipe_write);
    OPENSSL_cleanse(auth,sizeof(*auth));free(auth);
}

int nb_auth_async_context_result_fd(nb_auth_async_t* auth){return auth?auth->pipe_read:-1;}
int nb_auth_async_context_submit(nb_auth_async_t* auth,uint32_t session_id,const char* username,
    const unsigned char* password,size_t password_len){
    if(auth==NULL||session_id==0||username==NULL||username[0]==0||strlen(username)>=NB_AUTH_NAME_MAX||password==NULL||password_len==0||password_len>255)return -1;
    nb_auth_async_request_t request;memset(&request,0,sizeof(request));request.session_id=session_id;
    memcpy(request.username,username,strlen(username)+1);memcpy(request.password,password,password_len);request.password_len=password_len;
    if(!nb_auth_fingerprint(auth->users,username,password,password_len,request.fingerprint)){OPENSSL_cleanse(&request,sizeof(request));return -1;}
    pthread_mutex_lock(&auth->mutex);size_t next=next_slot(auth->request_tail);
    if(next==auth->request_head){pthread_mutex_unlock(&auth->mutex);OPENSSL_cleanse(&request,sizeof(request));return -1;}
    auth->requests[auth->request_tail]=request;auth->request_tail=next;pthread_cond_signal(&auth->request_ready);
    pthread_mutex_unlock(&auth->mutex);OPENSSL_cleanse(&request,sizeof(request));return 0;
}

int nb_auth_async_context_pop(nb_auth_async_t* auth,nb_auth_async_result_t* result){
    if(auth==NULL||result==NULL)return 0;
    pthread_mutex_lock(&auth->mutex);
    if(auth->result_head==auth->result_tail){pthread_mutex_unlock(&auth->mutex);return 0;}
    *result=auth->results[auth->result_head];memset(&auth->results[auth->result_head],0,sizeof(auth->results[auth->result_head]));
    auth->result_head=next_slot(auth->result_head);pthread_cond_signal(&auth->result_space);pthread_mutex_unlock(&auth->mutex);return 1;
}

int nb_auth_async_init(const nb_auth_users_t* users,int worker_count){if(legacy)return legacy->users==users?0:-1;legacy=nb_auth_async_create(users,worker_count);return legacy?0:-1;}
int nb_auth_async_result_fd(void){return nb_auth_async_context_result_fd(legacy);}
int nb_auth_async_submit(uint32_t id,const char* user,const unsigned char* pass,size_t len){return nb_auth_async_context_submit(legacy,id,user,pass,len);}
int nb_auth_async_pop(nb_auth_async_result_t* result){return nb_auth_async_context_pop(legacy,result);}
