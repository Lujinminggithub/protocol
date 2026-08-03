#ifndef NB_SHARD_H
#define NB_SHARD_H

#include <stddef.h>
#include <stdint.h>

#include "nb_instance.h"

#define NB_SHARD_MAX_INSTANCES 64
#define NB_SHARD_MAX_ARGS 64
#define NB_SHARD_ARG_MAX 512

typedef struct {
    char instance_id[65];
    char control_path[108];
    char arguments[NB_SHARD_MAX_ARGS][NB_SHARD_ARG_MAX];
    size_t argument_count;
    struct {char name[64];char value[512];} environment[NB_INSTANCE_ENV_MAX];
    size_t environment_count;
    uint64_t fingerprint;
    char source_path[512];
} nb_shard_config_t;

typedef int (*nb_shard_runner_fn)(int argc,char** argv,nb_instance_t* instance);

int nb_shard_config_load(const char* path,nb_shard_config_t* config,char* error,size_t error_cap);
int nb_shard_run(const char* directory,nb_shard_runner_fn runner);

#endif
