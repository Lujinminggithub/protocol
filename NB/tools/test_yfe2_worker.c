#include "nb_yfe2_worker.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(value) do { if(!(value)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1;} } while(0)

typedef struct {pthread_mutex_t mutex;pthread_cond_t cond;int entered,released;} barrier_t;
static int blocked_encode(const nb_yfe2_encode_job_t* job,nb_yfe2_encode_result_t* result,void* context){
    barrier_t* barrier=context;pthread_mutex_lock(&barrier->mutex);barrier->entered=1;
    pthread_cond_broadcast(&barrier->cond);while(!barrier->released)pthread_cond_wait(&barrier->cond,&barrier->mutex);
    pthread_mutex_unlock(&barrier->mutex);memset(result,0,sizeof(*result));
    result->connection_generation=job->connection_generation;result->session_id=job->session_id;
    result->block_id=job->block_id;return 0;
}

int main(void){
    barrier_t barrier={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0,0};
    nb_yfe2_worker_t* worker=nb_yfe2_worker_create(1,3*sizeof(nb_yfe2_encode_job_t),blocked_encode,&barrier);CHECK(worker);
    nb_yfe2_encode_job_t job={0};job.connection_generation=9;job.session_id=77;job.block_id=1;
    CHECK(nb_yfe2_worker_submit(worker,&job)==0);
    pthread_mutex_lock(&barrier.mutex);while(!barrier.entered)pthread_cond_wait(&barrier.cond,&barrier.mutex);pthread_mutex_unlock(&barrier.mutex);
    job.block_id=2;CHECK(nb_yfe2_worker_submit(worker,&job)==0);
    job.block_id=3;CHECK(nb_yfe2_worker_submit(worker,&job)==NB_YFE2_WORKER_FULL);
    pthread_mutex_lock(&barrier.mutex);barrier.released=1;pthread_cond_broadcast(&barrier.cond);pthread_mutex_unlock(&barrier.mutex);
    nb_yfe2_encode_result_t result;int found=0;
    for(int i=0;i<200&&!found;i++){while(nb_yfe2_worker_drain(worker,&result)==1){
        CHECK(result.connection_generation==9&&(result.block_id==1||result.block_id==2));found++;}usleep(1000);}
    CHECK(found==2&&nb_yfe2_worker_event_fd(worker)>=0);
    nb_yfe2_worker_metrics_t metrics;nb_yfe2_worker_snapshot(worker,&metrics);
    CHECK(metrics.submitted==2&&metrics.completed==2&&metrics.dropped==1);
    nb_yfe2_worker_destroy(worker);pthread_cond_destroy(&barrier.cond);pthread_mutex_destroy(&barrier.mutex);
    puts("nb_yfe2_worker_test: ok");return 0;
}
