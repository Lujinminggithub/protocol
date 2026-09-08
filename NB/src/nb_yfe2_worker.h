#ifndef NB_YFE2_WORKER_H
#define NB_YFE2_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "nb_yfe2_block.h"

#define NB_YFE2_WORKER_MAX_JOBS 128u
#define NB_YFE2_WORKER_MAX_BYTES (16u*1024u*1024u)
#define NB_YFE2_WORKER_FULL 1

typedef struct nb_yfe2_worker nb_yfe2_worker_t;
typedef int (*nb_yfe2_encode_fn)(const nb_yfe2_encode_job_t* job,
    nb_yfe2_encode_result_t* result,void* context);
typedef struct {uint64_t submitted,completed,dropped,encode_ns;size_t job_high,result_high,memory_high;} nb_yfe2_worker_metrics_t;

nb_yfe2_worker_t* nb_yfe2_worker_create(size_t max_jobs,size_t max_bytes,
    nb_yfe2_encode_fn encode,void* context);
void nb_yfe2_worker_destroy(nb_yfe2_worker_t* worker);
int nb_yfe2_worker_submit(nb_yfe2_worker_t* worker,const nb_yfe2_encode_job_t* job);
int nb_yfe2_worker_drain(nb_yfe2_worker_t* worker,nb_yfe2_encode_result_t* result);
int nb_yfe2_worker_event_fd(const nb_yfe2_worker_t* worker);
void nb_yfe2_worker_snapshot(nb_yfe2_worker_t* worker,nb_yfe2_worker_metrics_t* out);

#endif
