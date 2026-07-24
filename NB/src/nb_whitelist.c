#include "nb_whitelist.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define NB_WHITELIST_MAX 1024

typedef struct {uint32_t net,mask;} nb_whitelist_cidr_t;
typedef struct {
    char domains[NB_WHITELIST_MAX][128];int domain_count;
    nb_whitelist_cidr_t cidrs[NB_WHITELIST_MAX];int cidr_count;
    uint16_t ports[NB_WHITELIST_MAX];int port_count;
    int enabled;char path[256];time_t mtime;
} nb_whitelist_state_t;

static nb_whitelist_state_t state;

static int fail(char* out,size_t cap,const char* format,...){
    if(out&&cap){va_list ap;va_start(ap,format);vsnprintf(out,cap,format,ap);va_end(ap);}
    return -1;
}

static int load(const char* path,char* error,size_t error_cap){
    FILE* file=fopen(path,"r");if(!file)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    nb_whitelist_state_t next;memset(&next,0,sizeof(next));
    if(strlen(path)>=sizeof(next.path)){fclose(file);return fail(error,error_cap,"path too long");}
    strcpy(next.path,path);char line[512];
    while(fgets(line,sizeof(line),file)){
        char* p=line;while(*p==' '||*p=='\t')p++;
        if(*p=='#'||*p=='\n'||*p=='\r'||*p==0)continue;
        char keyword[16],value[256],extra[2];
        if(sscanf(p,"%15s %255s %1s",keyword,value,extra)!=2){fclose(file);return fail(error,error_cap,"invalid directive");}
        if(!strcmp(keyword,"domain")){
            size_t length=strlen(value);
            if(!length||length>=sizeof(next.domains[0])||next.domain_count>=NB_WHITELIST_MAX){fclose(file);return fail(error,error_cap,"invalid domain");}
            memcpy(next.domains[next.domain_count++],value,length+1);
        }else if(!strcmp(keyword,"port")){
            char* end=NULL;long port=strtol(value,&end,10);
            if(end==value||*end||port<=0||port>65535||next.port_count>=NB_WHITELIST_MAX){fclose(file);return fail(error,error_cap,"invalid port: %s",value);}
            next.ports[next.port_count++]=(uint16_t)port;
        }else if(!strcmp(keyword,"ip")){
            int prefix=32;char* slash=strchr(value,'/');
            if(slash){*slash=0;char* end=NULL;long parsed=strtol(slash+1,&end,10);if(end==slash+1||*end||parsed<0||parsed>32){fclose(file);return fail(error,error_cap,"invalid cidr prefix");}prefix=(int)parsed;}
            struct in_addr address;
            if(inet_pton(AF_INET,value,&address)!=1||next.cidr_count>=NB_WHITELIST_MAX){fclose(file);return fail(error,error_cap,"invalid cidr: %s",value);}
            uint32_t mask=prefix==0?0u:(0xffffffffu<<(32-prefix));
            next.cidrs[next.cidr_count].mask=mask;next.cidrs[next.cidr_count].net=ntohl(address.s_addr)&mask;next.cidr_count++;
        }else{fclose(file);return fail(error,error_cap,"unknown directive: %s",keyword);}
    }
    fclose(file);
    if(!next.domain_count&&!next.cidr_count)return fail(error,error_cap,"no domain or cidr rules");
    struct stat status;if(stat(path,&status)==0)next.mtime=status.st_mtime;
    next.enabled=1;state=next;return 0;
}

int nb_whitelist_init(const char* path,char* error,size_t error_cap){
    memset(&state,0,sizeof(state));
    return !path||!*path?0:load(path,error,error_cap);
}

int nb_whitelist_allowed(const char* host,int port){
    if(!state.enabled)return 1;
    int port_allowed=state.port_count==0;
    for(int i=0;i<state.port_count&&!port_allowed;i++)port_allowed=state.ports[i]==(uint16_t)port;
    if(!port_allowed||!host)return 0;
    struct in_addr address;
    if(inet_pton(AF_INET,host,&address)==1){
        if(!state.cidr_count)return 1;
        uint32_t ip=ntohl(address.s_addr);
        for(int i=0;i<state.cidr_count;i++)if((ip&state.cidrs[i].mask)==state.cidrs[i].net)return 1;
        return 0;
    }
    if(!state.domain_count)return 1;
    size_t host_length=strlen(host);
    for(int i=0;i<state.domain_count;i++){
        size_t length=strlen(state.domains[i]);
        if((host_length==length&&!strcasecmp(host,state.domains[i]))||
           (host_length>length&&host[host_length-length-1]=='.'&&!strcasecmp(host+host_length-length,state.domains[i])))return 1;
    }
    return 0;
}

int nb_whitelist_reload_if_changed(char* error,size_t error_cap){
    if(!state.path[0])return 0;
    struct stat status;
    if(stat(state.path,&status)!=0)return fail(error,error_cap,"stat %s: %s",state.path,strerror(errno));
    if(status.st_mtime==state.mtime)return 0;
    return load(state.path,error,error_cap)==0?1:-1;
}

int nb_whitelist_configured(void){return state.path[0]!=0;}
