#include "nb_tenant.h"
#include "nb_tenant_shared.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* out,size_t cap,const char* fmt,...){if(out&&cap){va_list ap;va_start(ap,fmt);vsnprintf(out,cap,fmt,ap);va_end(ap);}return -1;}
static int parse_u64(const char* text,uint64_t max,uint64_t* out){char* end=NULL;errno=0;unsigned long long v=strtoull(text,&end,10);if(errno||end==text||*end||v>max)return -1;*out=(uint64_t)v;return 0;}
static int valid_name(const char* text){if(!text||!*text)return 0;for(const unsigned char* p=(const unsigned char*)text;*p;p++)if(!( (*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='.'||*p=='-'))return 0;return 1;}

int nb_tenants_load(nb_tenants_t* out,const char* path,char* error,size_t error_cap){
    if(!out||!path)return fail(error,error_cap,"missing tenant config");
    FILE* f=fopen(path,"r");if(!f)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    nb_tenants_t next;memset(&next,0,sizeof(next));next.shared_fd=-1;char line[512];unsigned line_no=0;
    while(fgets(line,sizeof(line),f)){
        line_no++;char* p=line;while(*p==' '||*p=='\t')p++;if(*p==0||*p=='#'||*p=='\r'||*p=='\n')continue;
        char keyword[16],name[NB_TENANT_NAME_MAX],tcp[32],udp[32],rate[32],quota[32],extra[8];
        if(sscanf(p,"%15s %63s %31s %31s %31s %31s %7s",keyword,name,tcp,udp,rate,quota,extra)!=6||strcmp(keyword,"tenant")||!valid_name(name)||next.count>=NB_TENANT_MAX){fclose(f);return fail(error,error_cap,"invalid tenant at line %u",line_no);}
        uint64_t a,b,c,d;if(parse_u64(tcp,100000,&a)||parse_u64(udp,100000,&b)||parse_u64(rate,100000000,&c)||parse_u64(quota,UINT64_MAX/(1024ULL*1024ULL),&d)){fclose(f);return fail(error,error_cap,"invalid tenant limits at line %u",line_no);}
        for(size_t i=0;i<next.count;i++)if(!strcmp(next.items[i].name,name)){fclose(f);return fail(error,error_cap,"duplicate tenant at line %u",line_no);}
        nb_tenant_t* t=&next.items[next.count++];snprintf(t->name,sizeof(t->name),"%s",name);t->max_tcp=(uint32_t)a;t->max_udp=(uint32_t)b;t->rate_bytes_per_sec=c?c*1000ULL/8ULL:0;t->byte_quota=d*1024ULL*1024ULL;t->tokens=t->rate_bytes_per_sec;
    }
    fclose(f);if(next.count==0)return fail(error,error_cap,"tenant config is empty");*out=next;return 0;
}

int nb_tenants_enable_shared(nb_tenants_t* tenants,const char* path,char* error,size_t error_cap){return nb_tenant_shared_open(tenants,path,error,error_cap);}
void nb_tenants_close(nb_tenants_t* tenants){nb_tenant_shared_close(tenants);}

int nb_tenant_find(const nb_tenants_t* tenants,const char* name){if(!tenants||!name)return -1;for(size_t i=0;i<tenants->count;i++)if(!strcmp(tenants->items[i].name,name))return (int)i;return -1;}

int nb_tenant_acquire(nb_tenants_t* tenants,int index,int udp,uint64_t now_us){
    if(!tenants||index<0||(size_t)index>=tenants->count)return -1;
    if(tenants->shared_state)return nb_tenant_shared_acquire(tenants,index,udp,now_us);
    nb_tenant_t* t=&tenants->items[index];
    uint32_t* active=udp?&t->active_udp:&t->active_tcp;uint32_t limit=udp?t->max_udp:t->max_tcp;
    if((limit&&*active>=limit)||(t->byte_quota&&t->bytes_up+t->bytes_down>=t->byte_quota)){t->rejected++;return -1;}
    (*active)++;if(t->token_updated_us==0)t->token_updated_us=now_us;return 0;
}

void nb_tenant_release(nb_tenants_t* tenants,int index,int udp){if(!tenants||index<0||(size_t)index>=tenants->count)return;if(tenants->shared_state){nb_tenant_shared_release(tenants,index,udp);return;}uint32_t* active=udp?&tenants->items[index].active_udp:&tenants->items[index].active_tcp;if(*active)(*active)--;}

size_t nb_tenant_allowance(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us){
    if(!tenants||index<0||(size_t)index>=tenants->count)return requested;
    if(tenants->shared_state)return nb_tenant_shared_allowance(tenants,index,requested,now_us,0);
    nb_tenant_t* t=&tenants->items[index];
    if(t->byte_quota){uint64_t used=t->bytes_up+t->bytes_down;if(used>=t->byte_quota)return 0;uint64_t left=t->byte_quota-used;if(left<requested)requested=(size_t)left;}
    if(!t->rate_bytes_per_sec)return requested;
    if(!t->token_updated_us)t->token_updated_us=now_us;
    if(now_us>t->token_updated_us){uint64_t elapsed=now_us-t->token_updated_us;uint64_t add=elapsed>UINT64_MAX/t->rate_bytes_per_sec?UINT64_MAX:elapsed*t->rate_bytes_per_sec/1000000ULL;uint64_t burst=t->rate_bytes_per_sec;t->tokens=(add>=burst||t->tokens>=burst-add)?burst:t->tokens+add;t->token_updated_us=now_us;}
    if(t->tokens<requested)requested=(size_t)t->tokens;
    return requested;
}

void nb_tenant_consume(nb_tenants_t* tenants,int index,size_t bytes){if(!tenants||index<0||(size_t)index>=tenants->count||tenants->shared_state)return;nb_tenant_t* t=&tenants->items[index];t->tokens=t->tokens>bytes?t->tokens-bytes:0;}

size_t nb_tenant_take(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us){
    if(!tenants||index<0||(size_t)index>=tenants->count)return requested;
    if(tenants->shared_state)return nb_tenant_shared_allowance(tenants,index,requested,now_us,1);
    size_t allowed=nb_tenant_allowance(tenants,index,requested,now_us);nb_tenant_consume(tenants,index,allowed);return allowed;
}

void nb_tenant_refund(nb_tenants_t* tenants,int index,size_t bytes){
    if(!tenants||index<0||(size_t)index>=tenants->count||bytes==0)return;
    if(tenants->shared_state){nb_tenant_shared_refund(tenants,index,bytes);return;}
    nb_tenant_t* t=&tenants->items[index];uint64_t burst=t->rate_bytes_per_sec;
    if(burst)t->tokens=bytes>=burst||t->tokens>=burst-bytes?burst:t->tokens+bytes;
}

int64_t nb_tenant_wake_delay(uint64_t throttled_until_us,uint64_t now_us,
    int64_t current_delay_us){
    if(throttled_until_us==0)return current_delay_us;
    uint64_t raw=throttled_until_us<=now_us?0:throttled_until_us-now_us;
    int64_t delay=raw>(uint64_t)INT64_MAX?INT64_MAX:(int64_t)raw;
    return current_delay_us<0||delay<current_delay_us?delay:current_delay_us;
}

void nb_tenant_account(nb_tenants_t* tenants,int index,uint64_t up,uint64_t down){if(!tenants||index<0||(size_t)index>=tenants->count)return;if(tenants->shared_state){nb_tenant_shared_account(tenants,index,up,down);return;}tenants->items[index].bytes_up+=up;tenants->items[index].bytes_down+=down;}

int nb_tenants_render_json(const nb_tenants_t* tenants,char* out,size_t cap){
    if(!tenants||!out||cap==0)return -1;
    if(tenants->shared_state&&nb_tenant_shared_snapshot((nb_tenants_t*)tenants)!=0)return -1;
    size_t off=(size_t)snprintf(out,cap,"{\"tenants\":[");
    for(size_t i=0;i<tenants->count&&off<cap;i++){const nb_tenant_t* t=&tenants->items[i];int n=snprintf(out+off,cap-off,"%s{\"name\":\"%s\",\"active_tcp\":%u,\"active_udp\":%u,\"bytes_up\":%llu,\"bytes_down\":%llu,\"rejected\":%llu}",i?",":"",t->name,t->active_tcp,t->active_udp,(unsigned long long)t->bytes_up,(unsigned long long)t->bytes_down,(unsigned long long)t->rejected);if(n<0||(size_t)n>=cap-off)return -1;off+=(size_t)n;}
    int n=snprintf(out+off,cap-off,"]}\n");return n<0||(size_t)n>=cap-off?-1:(int)(off+(size_t)n);
}
