#include "nb_instance.h"

#include <stdlib.h>
#include <string.h>

#include "nb_udp.h"

void nb_instance_init(nb_instance_t* instance){
    if(instance==NULL)return;
    memset(instance,0,sizeof(*instance));
    instance->global.tcp_listen_fd=-1;
    instance->global.udp_fd=-1;
    instance->global.client_udp_fd=-1;
    instance->global.epoll_fd=-1;
    instance->global.control_fd=-1;
    instance->udp_gso_enabled=1;
    instance->reorder_gap=3;
    instance->pool_health_config=(nb_pool_health_config_t){750000ULL,5000000ULL,12000000ULL,30000000ULL};
    instance->fec_v15_observe=1;
    instance->fec_bdp_bitrate_bps=20000000ULL;
    instance->fec_bdp_rtt_us=250000ULL;
    instance->fec_bdp_multiplier=2;
    instance->udp_control_grace_us=NB_UDP_CONTROL_GRACE_US;
    instance->queue_limit_bytes=NB_INSTANCE_DEFAULT_QUEUE_LIMIT;
    instance->session_limit=NB_MAX_CONNECTIONS;
}

const char* nb_instance_env(const nb_instance_t* instance,const char* name){
    if(instance&&name)for(size_t i=0;i<instance->environment_count;i++)
        if(strcmp(instance->environment[i].name,name)==0)return instance->environment[i].value;
    return name?getenv(name):NULL;
}

int nb_instance_set_env(nb_instance_t* instance,const char* name,const char* value){
    if(instance==NULL||name==NULL||value==NULL||name[0]==0||strlen(name)>=sizeof(instance->environment[0].name)||
        strlen(value)>=sizeof(instance->environment[0].value))return -1;
    size_t index=instance->environment_count;
    for(size_t i=0;i<instance->environment_count;i++)if(strcmp(instance->environment[i].name,name)==0){index=i;break;}
    if(index==instance->environment_count){if(index>=NB_INSTANCE_ENV_MAX)return -1;instance->environment_count++;}
    memcpy(instance->environment[index].name,name,strlen(name)+1);
    memcpy(instance->environment[index].value,value,strlen(value)+1);return 0;
}
