#include "nb_tenant_shared.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NB_TENANT_SHARED_MAGIC 0x4e425453U
#define NB_TENANT_SHARED_VERSION 2U
#define NB_TENANT_SHARED_WORKERS 32

typedef struct {
    uint64_t bytes_up;
    uint64_t bytes_down;
    uint64_t rejected;
    uint64_t tokens[NB_TENANT_DIRECTIONS];
    uint64_t token_updated_us[NB_TENANT_DIRECTIONS];
} shared_tenant_t;

typedef struct {
    int64_t pid;
    uint32_t active_tcp[NB_TENANT_MAX];
    uint32_t active_udp[NB_TENANT_MAX];
} shared_worker_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t config_hash;
    uint32_t tenant_count;
    uint32_t reserved;
    shared_tenant_t tenants[NB_TENANT_MAX];
    shared_worker_t workers[NB_TENANT_SHARED_WORKERS];
} shared_state_t;

static int fail(char* out,size_t cap,const char* fmt,...){
    if(out&&cap){va_list ap;va_start(ap,fmt);vsnprintf(out,cap,fmt,ap);va_end(ap);}return -1;
}

static uint64_t config_hash(const nb_tenants_t* tenants){
    uint64_t h=1469598103934665603ULL;
    for(size_t i=0;i<tenants->count;i++){
        const nb_tenant_t* t=&tenants->items[i];
        const unsigned char* p=(const unsigned char*)t->name;
        while(*p){h^=*p++;h*=1099511628211ULL;}
        const uint64_t values[]={t->max_tcp,t->max_udp,t->rate_bytes_per_sec[NB_TENANT_UP],t->rate_bytes_per_sec[NB_TENANT_DOWN],t->burst_bytes[NB_TENANT_UP],t->burst_bytes[NB_TENANT_DOWN],t->byte_quota};
        for(size_t j=0;j<sizeof(values)/sizeof(values[0]);j++)for(unsigned k=0;k<8;k++){
            h^=(unsigned char)(values[j]>>(k*8));h*=1099511628211ULL;
        }
    }
    return h;
}

static int lock_state(nb_tenants_t* tenants){return tenants->shared_fd>=0?flock(tenants->shared_fd,LOCK_EX):-1;}
static void unlock_state(nb_tenants_t* tenants){if(tenants->shared_fd>=0)(void)flock(tenants->shared_fd,LOCK_UN);}

static int pid_alive(int64_t pid){
    if(pid<=0)return 0;
    if(kill((pid_t)pid,0)==0)return 1;
    return errno==EPERM;
}

static int worker_slot(shared_state_t* state,int create){
    int64_t self=(int64_t)getpid();int free_slot=-1;
    for(int i=0;i<NB_TENANT_SHARED_WORKERS;i++){
        if(state->workers[i].pid==self)return i;
        if(state->workers[i].pid!=0&&!pid_alive(state->workers[i].pid))memset(&state->workers[i],0,sizeof(state->workers[i]));
        if(free_slot<0&&state->workers[i].pid==0)free_slot=i;
    }
    if(create&&free_slot>=0)state->workers[free_slot].pid=self;
    return free_slot;
}

static void totals(shared_state_t* state,int index,uint32_t* tcp,uint32_t* udp){
    uint64_t a=0,b=0;
    for(int i=0;i<NB_TENANT_SHARED_WORKERS;i++)if(state->workers[i].pid!=0){
        a+=state->workers[i].active_tcp[index];b+=state->workers[i].active_udp[index];
    }
    *tcp=a>UINT32_MAX?UINT32_MAX:(uint32_t)a;*udp=b>UINT32_MAX?UINT32_MAX:(uint32_t)b;
}

static void refill(const nb_tenant_t* config,shared_tenant_t* state,uint64_t now_us,int direction){
    uint64_t rate=config->rate_bytes_per_sec[direction],burst=config->burst_bytes[direction];if(!rate)return;
    if(state->tokens[direction]>burst)state->tokens[direction]=burst;
    if(!state->token_updated_us[direction]){state->token_updated_us[direction]=now_us;state->tokens[direction]=burst;return;}
    if(now_us<=state->token_updated_us[direction])return;
    uint64_t elapsed=now_us-state->token_updated_us[direction];
    uint64_t add=elapsed>UINT64_MAX/rate?UINT64_MAX:elapsed*rate/1000000ULL;
    state->tokens[direction]=(add>=burst||state->tokens[direction]>=burst-add)?burst:state->tokens[direction]+add;
    state->token_updated_us[direction]=now_us;
}

int nb_tenant_shared_open(nb_tenants_t* tenants,const char* path,char* error,size_t error_cap){
    if(!tenants||!path||!*path)return fail(error,error_cap,"missing shared tenant state path");
    int fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC,0600);if(fd<0)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    if(flock(fd,LOCK_EX)!=0){close(fd);return fail(error,error_cap,"lock %s: %s",path,strerror(errno));}
    struct stat st;if(fstat(fd,&st)!=0||st.st_size!=(off_t)sizeof(shared_state_t)){
        if(ftruncate(fd,(off_t)sizeof(shared_state_t))!=0){flock(fd,LOCK_UN);close(fd);return fail(error,error_cap,"resize %s: %s",path,strerror(errno));}
    }
    shared_state_t* state=mmap(NULL,sizeof(*state),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(state==MAP_FAILED){flock(fd,LOCK_UN);close(fd);return fail(error,error_cap,"map %s: %s",path,strerror(errno));}
    uint64_t hash=config_hash(tenants);
    if(state->magic!=NB_TENANT_SHARED_MAGIC||state->version!=NB_TENANT_SHARED_VERSION||
       state->config_hash!=hash||state->tenant_count!=tenants->count){
        memset(state,0,sizeof(*state));state->magic=NB_TENANT_SHARED_MAGIC;
        state->version=NB_TENANT_SHARED_VERSION;state->config_hash=hash;state->tenant_count=(uint32_t)tenants->count;
        for(size_t i=0;i<tenants->count;i++)for(int d=0;d<NB_TENANT_DIRECTIONS;d++)state->tenants[i].tokens[d]=tenants->items[i].burst_bytes[d];
        (void)msync(state,sizeof(*state),MS_SYNC);
    }
    tenants->shared_fd=fd;tenants->shared_state=state;
    if(worker_slot(state,1)<0){munmap(state,sizeof(*state));tenants->shared_state=NULL;tenants->shared_fd=-1;
        flock(fd,LOCK_UN);close(fd);return fail(error,error_cap,"shared tenant worker slots exhausted");}
    flock(fd,LOCK_UN);return 0;
}

void nb_tenant_shared_close(nb_tenants_t* tenants){
    if(!tenants||tenants->shared_fd<0||!tenants->shared_state)return;
    shared_state_t* state=tenants->shared_state;
    if(lock_state(tenants)==0){int slot=worker_slot(state,0);if(slot>=0)memset(&state->workers[slot],0,sizeof(state->workers[slot]));unlock_state(tenants);}
    munmap(state,sizeof(*state));close(tenants->shared_fd);tenants->shared_state=NULL;tenants->shared_fd=-1;
}

int nb_tenant_shared_acquire(nb_tenants_t* tenants,int index,int udp,uint64_t now_us){
    if(lock_state(tenants)!=0)return -1;
    shared_state_t* state=tenants->shared_state;
    int slot=worker_slot(state,1);uint32_t tcp=0,datagrams=0;totals(state,index,&tcp,&datagrams);
    nb_tenant_t* config=&tenants->items[index];shared_tenant_t* usage=&state->tenants[index];
    uint32_t active=udp?datagrams:tcp,limit=udp?config->max_udp:config->max_tcp;
    int rejected=slot<0||(limit&&active>=limit)||(config->byte_quota&&usage->bytes_up+usage->bytes_down>=config->byte_quota);
    if(rejected)usage->rejected++;else {uint32_t* count=udp?&state->workers[slot].active_udp[index]:&state->workers[slot].active_tcp[index];(*count)++;for(int d=0;d<NB_TENANT_DIRECTIONS;d++)if(!usage->token_updated_us[d])usage->token_updated_us[d]=now_us;}
    unlock_state(tenants);return rejected?-1:0;
}

void nb_tenant_shared_release(nb_tenants_t* tenants,int index,int udp){
    if(lock_state(tenants)!=0)return;
    shared_state_t* state=tenants->shared_state;int slot=worker_slot(state,0);
    if(slot>=0){uint32_t* count=udp?&state->workers[slot].active_udp[index]:&state->workers[slot].active_tcp[index];if(*count)(*count)--;}
    unlock_state(tenants);
}

size_t nb_tenant_shared_allowance(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us,int take,int direction){
    if((direction!=NB_TENANT_UP&&direction!=NB_TENANT_DOWN)||lock_state(tenants)!=0)return 0;
    shared_state_t* state=tenants->shared_state;
    nb_tenant_t* config=&tenants->items[index];shared_tenant_t* usage=&state->tenants[index];
    if(config->byte_quota){uint64_t used=usage->bytes_up+usage->bytes_down;if(used>=config->byte_quota)requested=0;else if(config->byte_quota-used<requested)requested=(size_t)(config->byte_quota-used);}
    if(config->rate_bytes_per_sec[direction]){refill(config,usage,now_us,direction);if(usage->tokens[direction]<requested)requested=(size_t)usage->tokens[direction];if(take)usage->tokens[direction]-=requested;}
    unlock_state(tenants);return requested;
}

void nb_tenant_shared_refund(nb_tenants_t* tenants,int index,size_t bytes,int direction){
    if(bytes==0||(direction!=NB_TENANT_UP&&direction!=NB_TENANT_DOWN)||lock_state(tenants)!=0)return;
    shared_state_t* state=tenants->shared_state;
    uint64_t burst=tenants->items[index].burst_bytes[direction];shared_tenant_t* usage=&state->tenants[index];
    if(burst)usage->tokens[direction]=bytes>=burst||usage->tokens[direction]>=burst-bytes?burst:usage->tokens[direction]+bytes;
    unlock_state(tenants);
}

void nb_tenant_shared_account(nb_tenants_t* tenants,int index,uint64_t up,uint64_t down){
    if(lock_state(tenants)!=0)return;
    shared_tenant_t* usage=&((shared_state_t*)tenants->shared_state)->tenants[index];
    usage->bytes_up+=up;usage->bytes_down+=down;unlock_state(tenants);
}

int nb_tenant_shared_snapshot(nb_tenants_t* tenants){
    if(lock_state(tenants)!=0)return -1;
    shared_state_t* state=tenants->shared_state;
    (void)worker_slot(state,0);
    for(size_t i=0;i<tenants->count;i++){nb_tenant_t* local=&tenants->items[i];shared_tenant_t* usage=&state->tenants[i];
        totals(state,(int)i,&local->active_tcp,&local->active_udp);local->bytes_up=usage->bytes_up;local->bytes_down=usage->bytes_down;
        local->rejected=usage->rejected;for(int d=0;d<NB_TENANT_DIRECTIONS;d++){local->tokens[d]=usage->tokens[d];local->token_updated_us[d]=usage->token_updated_us[d];}}
    unlock_state(tenants);return 0;
}

#else
int nb_tenant_shared_open(nb_tenants_t* t,const char* p,char* e,size_t c){(void)t;(void)p;if(e&&c)snprintf(e,c,"shared tenant state requires POSIX");return -1;}
void nb_tenant_shared_close(nb_tenants_t* t){(void)t;}
int nb_tenant_shared_acquire(nb_tenants_t* t,int i,int u,uint64_t n){(void)t;(void)i;(void)u;(void)n;return -1;}
void nb_tenant_shared_release(nb_tenants_t* t,int i,int u){(void)t;(void)i;(void)u;}
size_t nb_tenant_shared_allowance(nb_tenants_t* t,int i,size_t r,uint64_t n,int k,int d){(void)t;(void)i;(void)r;(void)n;(void)k;(void)d;return 0;}
void nb_tenant_shared_refund(nb_tenants_t* t,int i,size_t b,int d){(void)t;(void)i;(void)b;(void)d;}
void nb_tenant_shared_account(nb_tenants_t* t,int i,uint64_t u,uint64_t d){(void)t;(void)i;(void)u;(void)d;}
int nb_tenant_shared_snapshot(nb_tenants_t* t){(void)t;return -1;}
#endif
