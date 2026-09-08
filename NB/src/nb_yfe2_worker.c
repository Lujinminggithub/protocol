#include "nb_yfe2_worker.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

struct nb_yfe2_worker {
    pthread_t thread;pthread_mutex_t mutex;pthread_cond_t cond;int stop,event_fd;
    size_t cap,result_cap,max_bytes,job_head,job_count,result_head,result_count;
    nb_yfe2_encode_job_t* jobs;nb_yfe2_encode_result_t* results;
    nb_yfe2_encode_fn encode;void* context;nb_yfe2_worker_metrics_t metrics;
};
static uint64_t monotonic_ns(void){struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;}
static int default_encode(const nb_yfe2_encode_job_t* job,nb_yfe2_encode_result_t* result,void* context){
    (void)context;return nb_yfe2_encode_job_run(job,result);}
static size_t memory_used(const nb_yfe2_worker_t* w){return w->job_count*sizeof(*w->jobs)+w->result_count*sizeof(*w->results);}
static void* worker_main(void* opaque){nb_yfe2_worker_t* w=opaque;
    for(;;){nb_yfe2_encode_job_t job;pthread_mutex_lock(&w->mutex);
        while(!w->stop&&w->job_count==0)pthread_cond_wait(&w->cond,&w->mutex);
        if(w->stop&&w->job_count==0){pthread_mutex_unlock(&w->mutex);break;}
        job=w->jobs[w->job_head];w->job_head=(w->job_head+1)%w->cap;w->job_count--;pthread_mutex_unlock(&w->mutex);
        nb_yfe2_encode_result_t result;uint64_t started=monotonic_ns();int rc=w->encode(&job,&result,w->context);uint64_t ended=monotonic_ns();
        pthread_mutex_lock(&w->mutex);w->metrics.encode_ns+=ended>=started?ended-started:0;
        if(rc==0&&w->result_count<w->result_cap&&memory_used(w)+sizeof(result)<=w->max_bytes){size_t index=(w->result_head+w->result_count)%w->result_cap;w->results[index]=result;w->result_count++;
            w->metrics.completed++;if(w->result_count>w->metrics.result_high)w->metrics.result_high=w->result_count;
            uint64_t one=1;ssize_t notified=write(w->event_fd,&one,sizeof(one));(void)notified;}else w->metrics.dropped++;
        size_t used=memory_used(w);if(used>w->metrics.memory_high)w->metrics.memory_high=used;pthread_mutex_unlock(&w->mutex);
    }return NULL;}
nb_yfe2_worker_t* nb_yfe2_worker_create(size_t max_jobs,size_t max_bytes,nb_yfe2_encode_fn encode,void* context){
    if(max_jobs==0||max_jobs>NB_YFE2_WORKER_MAX_JOBS||max_bytes<sizeof(nb_yfe2_encode_job_t)||max_bytes>NB_YFE2_WORKER_MAX_BYTES)return NULL;
    nb_yfe2_worker_t* w=calloc(1,sizeof(*w));if(!w)return NULL;w->cap=max_jobs;
    w->result_cap=max_jobs<NB_YFE2_WORKER_MAX_JOBS?max_jobs+1:max_jobs;w->max_bytes=max_bytes;
    w->encode=encode?encode:default_encode;w->context=context;w->event_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    w->jobs=calloc(max_jobs,sizeof(*w->jobs));w->results=calloc(w->result_cap,sizeof(*w->results));
    if(w->event_fd<0||!w->jobs||!w->results)goto fail;
    if(pthread_mutex_init(&w->mutex,NULL)!=0)goto fail;
    if(pthread_cond_init(&w->cond,NULL)!=0){pthread_mutex_destroy(&w->mutex);goto fail;}
    if(pthread_create(&w->thread,NULL,worker_main,w)!=0){pthread_cond_destroy(&w->cond);pthread_mutex_destroy(&w->mutex);goto fail;}
    return w;
fail:
    if(w->event_fd>=0)close(w->event_fd);
    free(w->jobs);free(w->results);free(w);return NULL;
}
void nb_yfe2_worker_destroy(nb_yfe2_worker_t* w){if(!w)return;pthread_mutex_lock(&w->mutex);w->stop=1;pthread_cond_broadcast(&w->cond);pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread,NULL);pthread_cond_destroy(&w->cond);pthread_mutex_destroy(&w->mutex);close(w->event_fd);free(w->jobs);free(w->results);free(w);}
int nb_yfe2_worker_submit(nb_yfe2_worker_t* w,const nb_yfe2_encode_job_t* job){if(!w||!job)return -1;pthread_mutex_lock(&w->mutex);
    size_t projected=memory_used(w)+sizeof(*job);if(w->job_count>=w->cap||projected>w->max_bytes){w->metrics.dropped++;pthread_mutex_unlock(&w->mutex);return NB_YFE2_WORKER_FULL;}
    size_t index=(w->job_head+w->job_count)%w->cap;w->jobs[index]=*job;w->job_count++;w->metrics.submitted++;
    if(w->job_count>w->metrics.job_high)w->metrics.job_high=w->job_count;
    if(projected>w->metrics.memory_high)w->metrics.memory_high=projected;
    pthread_cond_signal(&w->cond);pthread_mutex_unlock(&w->mutex);return 0;}
int nb_yfe2_worker_drain(nb_yfe2_worker_t* w,nb_yfe2_encode_result_t* result){if(!w||!result)return -1;pthread_mutex_lock(&w->mutex);
    if(w->result_count==0){pthread_mutex_unlock(&w->mutex);return 0;}*result=w->results[w->result_head];w->result_head=(w->result_head+1)%w->result_cap;w->result_count--;
    uint64_t counter;while(read(w->event_fd,&counter,sizeof(counter))>0){}pthread_mutex_unlock(&w->mutex);return 1;}
int nb_yfe2_worker_event_fd(const nb_yfe2_worker_t* worker){return worker?worker->event_fd:-1;}
void nb_yfe2_worker_snapshot(nb_yfe2_worker_t* w,nb_yfe2_worker_metrics_t* out){if(!out)return;if(!w){memset(out,0,sizeof(*out));return;}
    pthread_mutex_lock(&w->mutex);*out=w->metrics;pthread_mutex_unlock(&w->mutex);}
