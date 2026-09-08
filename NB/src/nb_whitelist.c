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

#define NB_WHITELIST_MAX 8192

typedef struct {uint32_t net,mask;} nb_whitelist_cidr_t;
struct nb_whitelist {
    char domain_suffixes[NB_WHITELIST_MAX][128];int domain_suffix_count;
    char domain_exact[NB_WHITELIST_MAX][128];int domain_exact_count;
    char domain_keywords[NB_WHITELIST_MAX][128];int domain_keyword_count;
    nb_whitelist_cidr_t cidrs[NB_WHITELIST_MAX];int cidr_count;
    uint16_t ports[NB_WHITELIST_MAX];int port_count;
    int enabled;char path[256];time_t mtime;
};

static _Thread_local nb_whitelist_t* legacy_state;

static int fail(char* out,size_t cap,const char* format,...){
    if(out&&cap){va_list ap;va_start(ap,format);vsnprintf(out,cap,format,ap);va_end(ap);}
    return -1;
}

static int load(nb_whitelist_t* state,const char* path,char* error,size_t error_cap){
    FILE* file=fopen(path,"r");if(!file)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    nb_whitelist_t next;memset(&next,0,sizeof(next));
    if(strlen(path)>=sizeof(next.path)){fclose(file);return fail(error,error_cap,"path too long");}
    strcpy(next.path,path);char line[512];
    while(fgets(line,sizeof(line),file)){
        char* p=line;while(*p==' '||*p=='\t')p++;
        if(*p=='#'||*p=='\n'||*p=='\r'||*p==0)continue;
        char keyword[16],value[256],extra[2];
        if(sscanf(p,"%15s %255s %1s",keyword,value,extra)!=2){fclose(file);return fail(error,error_cap,"invalid directive");}
        if(!strcmp(keyword,"domain")||!strcmp(keyword,"domain_suffix")){
            size_t length=strlen(value);
            if(!length||length>=sizeof(next.domain_suffixes[0])||next.domain_suffix_count>=NB_WHITELIST_MAX){fclose(file);return fail(error,error_cap,"invalid domain suffix");}
            memcpy(next.domain_suffixes[next.domain_suffix_count++],value,length+1);
        }else if(!strcmp(keyword,"domain_exact")){
            size_t length=strlen(value);
            if(!length||length>=sizeof(next.domain_exact[0])||next.domain_exact_count>=NB_WHITELIST_MAX){fclose(file);return fail(error,error_cap,"invalid exact domain");}
            memcpy(next.domain_exact[next.domain_exact_count++],value,length+1);
        }else if(!strcmp(keyword,"domain_keyword")){
            size_t length=strlen(value);
            if(!length||length>=sizeof(next.domain_keywords[0])||next.domain_keyword_count>=NB_WHITELIST_MAX){fclose(file);return fail(error,error_cap,"invalid domain keyword");}
            memcpy(next.domain_keywords[next.domain_keyword_count++],value,length+1);
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
    if(!next.domain_suffix_count&&!next.domain_exact_count&&!next.domain_keyword_count&&!next.cidr_count)return fail(error,error_cap,"no domain or cidr rules");
    struct stat status;if(stat(path,&status)==0)next.mtime=status.st_mtime;
    next.enabled=1;*state=next;return 0;
}

nb_whitelist_t* nb_whitelist_create(const char* path,char* error,size_t error_cap){
    nb_whitelist_t* state=calloc(1,sizeof(*state));if(state==NULL){fail(error,error_cap,"allocation failed");return NULL;}
    if(path&&*path&&load(state,path,error,error_cap)!=0){free(state);return NULL;}
    return state;
}

void nb_whitelist_destroy(nb_whitelist_t* whitelist){free(whitelist);}

int nb_whitelist_context_allowed(const nb_whitelist_t* state,const char* host,int port){
    if(state==NULL||!state->enabled)return 1;
    if(host==NULL||port<=0||port>65535)return 0;
    if(port>=NB_WHITELIST_OPEN_PORT_MIN&&port<=NB_WHITELIST_OPEN_PORT_MAX)return 1;
    int port_allowed=state->port_count==0;
    for(int i=0;i<state->port_count&&!port_allowed;i++)port_allowed=state->ports[i]==(uint16_t)port;
    if(!port_allowed)return 0;
    struct in_addr address;
    if(inet_pton(AF_INET,host,&address)==1){
        if(!state->cidr_count)return 1;
        uint32_t ip=ntohl(address.s_addr);
        for(int i=0;i<state->cidr_count;i++)if((ip&state->cidrs[i].mask)==state->cidrs[i].net)return 1;
        return 0;
    }
    if(!state->domain_suffix_count&&!state->domain_exact_count&&!state->domain_keyword_count)return 1;
    size_t host_length=strlen(host);
    for(int i=0;i<state->domain_exact_count;i++)if(!strcasecmp(host,state->domain_exact[i]))return 1;
    for(int i=0;i<state->domain_keyword_count;i++){
        size_t length=strlen(state->domain_keywords[i]);
        for(size_t offset=0;offset+length<=host_length;offset++)
            if(!strncasecmp(host+offset,state->domain_keywords[i],length))return 1;
    }
    for(int i=0;i<state->domain_suffix_count;i++){
        size_t length=strlen(state->domain_suffixes[i]);
        if((host_length==length&&!strcasecmp(host,state->domain_suffixes[i]))||
           (host_length>length&&host[host_length-length-1]=='.'&&!strcasecmp(host+host_length-length,state->domain_suffixes[i])))return 1;
    }
    return 0;
}

int nb_whitelist_context_reload_if_changed(nb_whitelist_t* state,char* error,size_t error_cap){
    if(state==NULL||!state->path[0])return 0;
    struct stat status;
    if(stat(state->path,&status)!=0)return fail(error,error_cap,"stat %s: %s",state->path,strerror(errno));
    if(status.st_mtime==state->mtime)return 0;
    return load(state,state->path,error,error_cap)==0?1:-1;
}

int nb_whitelist_context_configured(const nb_whitelist_t* state){return state&&state->path[0]!=0;}

int nb_whitelist_init(const char* path,char* error,size_t error_cap){
    nb_whitelist_t* next=nb_whitelist_create(path,error,error_cap);if(next==NULL)return -1;
    nb_whitelist_destroy(legacy_state);legacy_state=next;return 0;
}
int nb_whitelist_allowed(const char* host,int port){return nb_whitelist_context_allowed(legacy_state,host,port);}
int nb_whitelist_reload_if_changed(char* error,size_t error_cap){return nb_whitelist_context_reload_if_changed(legacy_state,error,error_cap);}
int nb_whitelist_configured(void){return nb_whitelist_context_configured(legacy_state);}
