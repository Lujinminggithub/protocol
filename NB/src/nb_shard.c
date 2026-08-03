#define _POSIX_C_SOURCE 200809L
#include "nb_shard.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log/log4c.h"

typedef struct {
    nb_shard_config_t config;
    nb_instance_t* instance;
    pthread_t thread;
    nb_shard_runner_fn runner;
    int thread_started;
} nb_shard_slot_t;

static volatile sig_atomic_t shard_stop;
static volatile sig_atomic_t shard_reload;

static int fail(char* error,size_t cap,const char* format,...){
    if(error&&cap){va_list args;va_start(args,format);vsnprintf(error,cap,format,args);va_end(args);}return -1;
}

static int safe_id(const char* value,size_t limit){
    size_t length=value?strlen(value):0;if(length==0||length>limit)return 0;
    for(size_t i=0;i<length;i++)if(!(isalnum((unsigned char)value[i])||value[i]=='_'||value[i]=='-'||value[i]=='.'))return 0;
    return 1;
}

static int safe_env_name(const char* value){
    size_t length=value?strlen(value):0;if(length==0||length>=64)return 0;
    for(size_t i=0;i<length;i++)if(!(value[i]=='_'||(value[i]>='A'&&value[i]<='Z')||(i>0&&value[i]>='0'&&value[i]<='9')))return 0;
    return 1;
}

static uint64_t hash_bytes(const unsigned char* data,size_t length){
    uint64_t hash=1469598103934665603ULL;
    for(size_t i=0;i<length;i++){hash^=data[i];hash*=1099511628211ULL;}return hash;
}

static char* trim(char* value){
    while(*value==' '||*value=='\t')value++;
    size_t length=strlen(value);while(length&&strchr(" \t\r\n",value[length-1]))value[--length]=0;return value;
}

int nb_shard_config_load(const char* path,nb_shard_config_t* config,char* error,size_t error_cap){
    if(path==NULL||config==NULL)return fail(error,error_cap,"invalid shard config path");
    struct stat status;if(stat(path,&status)!=0||!S_ISREG(status.st_mode))return fail(error,error_cap,"config is not a regular file");
    if(status.st_mode&(S_IWGRP|S_IWOTH))return fail(error,error_cap,"config must not be group/world writable");
    FILE* file=fopen(path,"rb");if(file==NULL)return fail(error,error_cap,"open config: %s",strerror(errno));
    if(fseek(file,0,SEEK_END)!=0){fclose(file);return fail(error,error_cap,"seek config failed");}
    long size=ftell(file);if(size<1||size>65536||fseek(file,0,SEEK_SET)!=0){fclose(file);return fail(error,error_cap,"config size is invalid");}
    unsigned char* bytes=malloc((size_t)size+1);if(bytes==NULL){fclose(file);return fail(error,error_cap,"config allocation failed");}
    if(fread(bytes,1,(size_t)size,file)!=(size_t)size){free(bytes);fclose(file);return fail(error,error_cap,"read config failed");}
    fclose(file);bytes[size]=0;memset(config,0,sizeof(*config));config->fingerprint=hash_bytes(bytes,(size_t)size);
    if(strlen(path)>=sizeof(config->source_path)){free(bytes);return fail(error,error_cap,"config path is too long");}
    memcpy(config->source_path,path,strlen(path)+1);int schema=0;
    char* save=NULL;for(char* line=strtok_r((char*)bytes,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
        line=trim(line);if(*line==0||*line=='#')continue;char* separator=strchr(line,'=');
        if(separator==NULL){free(bytes);return fail(error,error_cap,"config directive is missing '='");}
        *separator=0;char* key=trim(line);char* value=trim(separator+1);
        if(strcmp(key,"schema")==0){schema=atoi(value);continue;}
        if(strcmp(key,"instance_id")==0){if(!safe_id(value,64)){free(bytes);return fail(error,error_cap,"invalid instance id");}memcpy(config->instance_id,value,strlen(value)+1);continue;}
        if(strcmp(key,"control_path")==0){if(value[0]!='/'||strlen(value)>=sizeof(config->control_path)){free(bytes);return fail(error,error_cap,"invalid control path");}memcpy(config->control_path,value,strlen(value)+1);continue;}
        if(strcmp(key,"arg")==0){if(config->argument_count>=NB_SHARD_MAX_ARGS||value[0]==0||strlen(value)>=NB_SHARD_ARG_MAX){free(bytes);return fail(error,error_cap,"invalid or excessive argument");}memcpy(config->arguments[config->argument_count++],value,strlen(value)+1);continue;}
        if(strncmp(key,"env.",4)==0){const char* name=key+4;if(!safe_env_name(name)||config->environment_count>=NB_INSTANCE_ENV_MAX||strlen(value)>=sizeof(config->environment[0].value)){free(bytes);return fail(error,error_cap,"invalid environment override");}
            size_t at=config->environment_count++;memcpy(config->environment[at].name,name,strlen(name)+1);memcpy(config->environment[at].value,value,strlen(value)+1);continue;}
        free(bytes);return fail(error,error_cap,"unknown config directive: %s",key);
    }
    free(bytes);
    if(schema!=1||config->instance_id[0]==0||config->control_path[0]==0||config->argument_count==0)return fail(error,error_cap,"config requires schema=1, instance_id, control_path and arguments");
    return 0;
}

static void signal_handler(int signal_number){if(signal_number==SIGHUP)shard_reload=1;else shard_stop=1;}

static void* instance_thread(void* context){
    nb_shard_slot_t* slot=context;nb_instance_t* instance=slot->instance;
    char* argv[NB_SHARD_MAX_ARGS+4];int argc=0;argv[argc++]="nb_node";
    for(size_t i=0;i<slot->config.argument_count;i++)argv[argc++]=slot->config.arguments[i];
    argv[argc++]="-C";argv[argc++]=slot->config.control_path;argv[argc]=NULL;
    instance->exit_code=slot->runner(argc,argv,instance);atomic_store(&instance->running,0);return NULL;
}

static int start_slot(nb_shard_slot_t* slot,const nb_shard_config_t* config,nb_shard_runner_fn runner){
    memset(slot,0,sizeof(*slot));slot->config=*config;slot->runner=runner;
    slot->instance=calloc(1,sizeof(*slot->instance));if(slot->instance==NULL)return -1;
    nb_instance_init(slot->instance);slot->instance->managed=1;
    if(nb_instance_set_env(slot->instance,"NB_INSTANCE_ID",config->instance_id)!=0)goto fail;
    for(size_t i=0;i<config->environment_count;i++)if(nb_instance_set_env(slot->instance,config->environment[i].name,config->environment[i].value)!=0)goto fail;
    atomic_store(&slot->instance->running,1);
    if(pthread_create(&slot->thread,NULL,instance_thread,slot)!=0)goto fail;
    slot->thread_started=1;
    for(int wait=0;wait<150&&!atomic_load(&slot->instance->ready)&&atomic_load(&slot->instance->running);wait++){
        struct timespec pause={.tv_sec=0,.tv_nsec=100000000};nanosleep(&pause,NULL);
    }
    if(!atomic_load(&slot->instance->ready))return -1;
    log4c_info("shard instance ready id=%s config=%s",config->instance_id,config->source_path);return 0;
fail:
    free(slot->instance);slot->instance=NULL;return -1;
}

static void stop_slot(nb_shard_slot_t* slot){
    if(slot->instance)atomic_store(&slot->instance->stop_requested,1);
    if(slot->thread_started)pthread_join(slot->thread,NULL);
    if(slot->instance)log4c_info("shard instance stopped id=%s status=%d",slot->config.instance_id,slot->instance->exit_code);
    free(slot->instance);memset(slot,0,sizeof(*slot));
}

static int config_name(const char* name){size_t length=strlen(name);return length>5&&strcmp(name+length-5,".conf")==0;}

static int scan_configs(const char* directory,nb_shard_config_t* configs,size_t* count){
    DIR* dir=opendir(directory);if(dir==NULL){log4c_error("shard config directory unavailable: %s",strerror(errno));return -1;}
    *count=0;struct dirent* item;
    while((item=readdir(dir))!=NULL){if(!config_name(item->d_name))continue;
        if(*count>=NB_SHARD_MAX_INSTANCES){closedir(dir);log4c_error("shard config limit exceeded");return -1;}
        char path[512];int n=snprintf(path,sizeof(path),"%s/%s",directory,item->d_name);
        if(n<0||(size_t)n>=sizeof(path)){closedir(dir);return -1;}char error[256];
        if(nb_shard_config_load(path,&configs[*count],error,sizeof(error))!=0){log4c_error("shard config rejected path=%s error=%s",path,error);closedir(dir);return -1;}
        for(size_t i=0;i<*count;i++)if(strcmp(configs[i].instance_id,configs[*count].instance_id)==0){closedir(dir);log4c_error("duplicate shard instance id=%s",configs[i].instance_id);return -1;}
        (*count)++;
    }
    closedir(dir);return 0;
}

static int find_config(const nb_shard_config_t* configs,size_t count,const char* id){for(size_t i=0;i<count;i++)if(strcmp(configs[i].instance_id,id)==0)return (int)i;return -1;}
static int find_slot(const nb_shard_slot_t* slots,size_t count,const char* id){for(size_t i=0;i<count;i++)if(strcmp(slots[i].config.instance_id,id)==0)return (int)i;return -1;}

static const char* config_role(const nb_shard_config_t* config){
    const char* role=NULL;
    for(size_t i=0;i+1<config->argument_count;i++){
        if(strcmp(config->arguments[i],"-r")==0)role=config->arguments[i+1];
    }
    return role;
}

static int config_listen_port(const nb_shard_config_t* config){
    const char* role=config_role(config);int port=0;
    const char* option=role&&strcmp(role,"entry")==0?"-l":
        role&&(strcmp(role,"middle")==0||strcmp(role,"exit")==0)?"-p":NULL;
    if(option==NULL)return -1;
    for(size_t i=0;i+1<config->argument_count;i++)if(strcmp(config->arguments[i],option)==0){
        char* end=NULL;long value=strtol(config->arguments[i+1],&end,10);
        if(end==config->arguments[i+1]||*end||value<1||value>65535)return -1;
        port=(int)value;
    }
    return port>0?port:-1;
}

static int directory_identity(const char* directory,char* role,size_t role_cap,int* worker,char* role_root,size_t root_cap){
    char path[512];size_t length=strlen(directory);if(length==0||length>=sizeof(path))return -1;
    memcpy(path,directory,length+1);while(length>1&&path[length-1]=='/')path[--length]=0;
    char* leaf=strrchr(path,'/');if(leaf==NULL)return -1;*leaf++=0;
    char* end=NULL;long value=strtol(leaf,&end,10);
    const char* selected=NULL;
    if(end!=leaf&&*end==0&&value>=0&&value<=31){
        char* parent=strrchr(path,'/');if(parent==NULL)return -1;selected=parent+1;*parent=0;
        if(snprintf(role_root,root_cap,"%s/%s",path,selected)<0)return -1;
        *worker=(int)value;
    }else{selected=leaf;if(snprintf(role_root,root_cap,"%s/%s",path,selected)<0)return -1;*worker=0;}
    if(strcmp(selected,"entry")&&strcmp(selected,"middle")&&strcmp(selected,"exit"))return -1;
    if(strlen(selected)>=role_cap)return -1;
    memcpy(role,selected,strlen(selected)+1);return 0;
}

typedef struct {int port;char instance_id[65];} nb_shard_port_claim_t;

static int validate_port_registry(const char* directory,const char* expected_role){
    char role[16],root[512];int local_worker=0;
    if(directory_identity(directory,role,sizeof(role),&local_worker,root,sizeof(root))!=0||strcmp(role,expected_role))return -1;
    nb_shard_port_claim_t* claims=calloc(NB_SHARD_MAX_INSTANCES*32,sizeof(*claims));if(claims==NULL)return -1;
    size_t claim_count=0;DIR* workers=opendir(root);
    if(workers==NULL){free(claims);return -1;}
    struct dirent* worker_item;
    while((worker_item=readdir(workers))!=NULL){
        char* end=NULL;long worker=strtol(worker_item->d_name,&end,10);
        if(end==worker_item->d_name||*end||worker<0||worker>31)continue;
        char worker_path[512];int n=snprintf(worker_path,sizeof(worker_path),"%s/%s",root,worker_item->d_name);
        if(n<0||(size_t)n>=sizeof(worker_path))goto fail;
        nb_shard_config_t* configs=calloc(NB_SHARD_MAX_INSTANCES,sizeof(*configs));if(configs==NULL)goto fail;
        size_t count=0;if(scan_configs(worker_path,configs,&count)!=0){free(configs);goto fail;}
        for(size_t i=0;i<count;i++){
            const char* configured_role=config_role(&configs[i]);int base=config_listen_port(&configs[i]);
            if(configured_role==NULL||strcmp(configured_role,expected_role)||base<1||
                (strcmp(expected_role,"entry")&&base+worker>65535)){free(configs);goto fail;}
            /* Entry workers intentionally share a line's SOCKS listener with
             * SO_REUSEPORT. Middle and exit workers use separate lane ports. */
            int actual=!strcmp(expected_role,"entry")?base:base+(int)worker;
            for(size_t j=0;j<claim_count;j++)if(claims[j].port==actual){
                if(!strcmp(expected_role,"entry")&&
                    !strcmp(claims[j].instance_id,configs[i].instance_id))continue;
                log4c_error("shard actual listen port conflict role=%s port=%d instances=%s,%s",
                    expected_role,actual,claims[j].instance_id,configs[i].instance_id);free(configs);goto fail;
            }
            claims[claim_count].port=actual;memcpy(claims[claim_count++].instance_id,
                configs[i].instance_id,strlen(configs[i].instance_id)+1);
        }
        free(configs);
    }
    closedir(workers);free(claims);return 0;
fail:
    closedir(workers);free(claims);return -1;
}

static int reconcile(const char* directory,const char* expected_role,nb_shard_slot_t* slots,size_t* slot_count,nb_shard_runner_fn runner){
    nb_shard_config_t* configs=calloc(NB_SHARD_MAX_INSTANCES,sizeof(*configs));if(configs==NULL)return -1;
    size_t config_count=0;if(scan_configs(directory,configs,&config_count)!=0||
        validate_port_registry(directory,expected_role)!=0){free(configs);return -1;}
    for(size_t i=0;i<config_count;i++){
        if(config_role(&configs[i])==NULL||strcmp(config_role(&configs[i]),expected_role)){
            log4c_error("shard role mismatch expected=%s instance=%s",expected_role,configs[i].instance_id);free(configs);return -1;
        }
        int port=config_listen_port(&configs[i]);
        if(port<0){log4c_error("shard config has no valid role listen port id=%s",configs[i].instance_id);free(configs);return -1;}
        for(size_t j=0;j<i;j++)if(config_listen_port(&configs[j])==port){
            log4c_error("shard listen port conflict port=%d instances=%s,%s",port,
                configs[j].instance_id,configs[i].instance_id);free(configs);return -1;
        }
    }
    for(size_t i=*slot_count;i>0;i--){size_t at=i-1;int found=find_config(configs,config_count,slots[at].config.instance_id);
        if(found<0){stop_slot(&slots[at]);if(at+1<*slot_count)memmove(&slots[at],&slots[at+1],(*slot_count-at-1)*sizeof(slots[0]));(*slot_count)--;continue;}
        if(configs[found].fingerprint!=slots[at].config.fingerprint||!atomic_load(&slots[at].instance->running)){
            /* The control socket pathname is owned by exactly one generation. Starting the
             * replacement first would unlink the live generation's socket in nb_control_open. */
            log4c_info("shard instance config changed; controlled restart id=%s",configs[found].instance_id);
            stop_slot(&slots[at]);
            if(start_slot(&slots[at],&configs[found],runner)!=0){
                if(slots[at].instance)stop_slot(&slots[at]);
                log4c_error("shard replacement failed id=%s; retrying on next reconciliation",configs[found].instance_id);
                if(at+1<*slot_count)memmove(&slots[at],&slots[at+1],(*slot_count-at-1)*sizeof(slots[0]));
                (*slot_count)--;
            }
        }
    }
    for(size_t i=0;i<config_count;i++)if(find_slot(slots,*slot_count,configs[i].instance_id)<0){
        if(start_slot(&slots[*slot_count],&configs[i],runner)==0)(*slot_count)++;
        else {if(slots[*slot_count].instance)stop_slot(&slots[*slot_count]);log4c_error("shard instance start failed id=%s",configs[i].instance_id);}
    }
    free(configs);return 0;
}

int nb_shard_run(const char* directory,nb_shard_runner_fn runner){
    if(directory==NULL||directory[0]!='/'||runner==NULL)return 1;
    char expected_role[16],role_root[512];int worker=0;
    if(directory_identity(directory,expected_role,sizeof(expected_role),&worker,role_root,sizeof(role_root))!=0)return 1;
    log4c_init("nb-shard");signal(SIGPIPE,SIG_IGN);signal(SIGTERM,signal_handler);signal(SIGINT,signal_handler);signal(SIGHUP,signal_handler);
    nb_shard_slot_t* slots=calloc(NB_SHARD_MAX_INSTANCES,sizeof(*slots));if(slots==NULL){log4c_shutdown();return 1;}
    size_t slot_count=0;if(reconcile(directory,expected_role,slots,&slot_count,runner)!=0){free(slots);log4c_shutdown();return 1;}
    log4c_info("shard runtime active directory=%s instances=%zu",directory,slot_count);
    while(!shard_stop){for(int i=0;i<20&&!shard_stop&&!shard_reload;i++){struct timespec pause={.tv_sec=0,.tv_nsec=100000000};nanosleep(&pause,NULL);}shard_reload=0;(void)reconcile(directory,expected_role,slots,&slot_count,runner);}
    while(slot_count>0)stop_slot(&slots[--slot_count]);
    free(slots);log4c_shutdown();return 0;
}
