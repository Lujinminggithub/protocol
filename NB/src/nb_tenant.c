#include "nb_tenant.h"
#include "nb_tenant_shared.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* out,size_t cap,const char* fmt,...){if(out&&cap){va_list ap;va_start(ap,fmt);vsnprintf(out,cap,fmt,ap);va_end(ap);}return -1;}
static int parse_u64(const char* text,uint64_t max,uint64_t* out){char* end=NULL;errno=0;unsigned long long v=strtoull(text,&end,10);if(errno||end==text||*end||v>max)return -1;*out=(uint64_t)v;return 0;}
static int valid_name(const char* text){if(!text||!*text)return 0;for(const unsigned char* p=(const unsigned char*)text;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='.'||*p=='-'))return 0;return 1;}
static int valid_direction(int direction){return direction==NB_TENANT_UP||direction==NB_TENANT_DOWN;}
static uint64_t media_reserve_bytes(const nb_tenant_t* tenant,int direction){
    uint64_t rate=tenant->rate_bytes_per_sec[direction];
    uint64_t reserve=(rate/1000000ULL)*NB_TENANT_MEDIA_RESERVE_US+
        (rate%1000000ULL)*NB_TENANT_MEDIA_RESERVE_US/1000000ULL;
    uint64_t burst=tenant->burst_bytes[direction];
    return burst&&reserve>burst?burst:reserve;
}

uint64_t nb_tenants_fingerprint(const nb_tenants_t* tenants){
    uint64_t h=1469598103934665603ULL;if(!tenants)return 0;
    for(size_t i=0;i<tenants->count;i++){
        const nb_tenant_t* t=&tenants->items[i];const unsigned char* p=(const unsigned char*)t->name;
        while(*p){h^=*p++;h*=1099511628211ULL;}
        const uint64_t values[]={t->max_tcp,t->max_udp,t->rate_bytes_per_sec[NB_TENANT_UP],
            t->rate_bytes_per_sec[NB_TENANT_DOWN],t->burst_bytes[NB_TENANT_UP],
            t->burst_bytes[NB_TENANT_DOWN],t->byte_quota};
        for(size_t j=0;j<sizeof(values)/sizeof(values[0]);j++)for(unsigned k=0;k<8;k++){
            h^=(unsigned char)(values[j]>>(k*8));h*=1099511628211ULL;
        }
    }
    return h;
}

void nb_tenant_refill(uint64_t rate,uint64_t burst,uint64_t now_us,
    uint64_t* tokens,uint64_t* updated_us,uint64_t* fraction){
    if(!rate||!tokens||!updated_us||!fraction)return;
    if(*tokens>=burst){*tokens=burst;*fraction=0;*updated_us=now_us;return;}
    if(!*updated_us){*tokens=burst;*fraction=0;*updated_us=now_us;return;}
    if(now_us<=*updated_us)return;
    uint64_t elapsed=now_us-*updated_us,missing=burst-*tokens;
    uint64_t seconds=elapsed/1000000ULL,micros=elapsed%1000000ULL;
    uint64_t seconds_to_fill=missing/rate+(missing%rate!=0);
    if(seconds>=seconds_to_fill){*tokens=burst;*fraction=0;*updated_us=now_us;return;}
    uint64_t whole=seconds*rate;*tokens+=whole;missing-=whole;
    uint64_t numerator=micros*rate+*fraction,add=numerator/1000000ULL;
    if(add>=missing){*tokens=burst;*fraction=0;}
    else {*tokens+=add;*fraction=numerator%1000000ULL;}
    *updated_us=now_us;
}

int nb_tenants_load(nb_tenants_t* out,const char* path,char* error,size_t error_cap){
    if(!out||!path)return fail(error,error_cap,"missing tenant config");
    FILE* f=fopen(path,"r");if(!f)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    nb_tenants_t next;memset(&next,0,sizeof(next));next.shared_fd=-1;char line[512];unsigned line_no=0;
    while(fgets(line,sizeof(line),f)){
        line_no++;char* p=line;while(*p==' '||*p=='\t')p++;if(*p==0||*p=='#'||*p=='\r'||*p=='\n')continue;
        char keyword[16],name[NB_TENANT_NAME_MAX],tcp[32],udp[32],rate_up[32],rate_down[32],quota[32],burst_up[32],burst_down[32],extra[8];
        int fields=sscanf(p,"%15s %63s %31s %31s %31s %31s %31s %31s %31s %7s",keyword,name,tcp,udp,rate_up,rate_down,quota,burst_up,burst_down,extra);
        int explicit_burst=!strcmp(keyword,"tenant-v3");
        if((fields!=6&&fields!=7&&fields!=9)||(!explicit_burst&&strcmp(keyword,"tenant"))||
           (explicit_burst&&fields!=9)||!valid_name(name)||next.count>=NB_TENANT_MAX){
            fclose(f);return fail(error,error_cap,"invalid tenant at line %u",line_no);}
        uint64_t a=0,b=0,up=0,down=0,q=0;
        uint64_t bu=NB_TENANT_DEFAULT_BURST_SECONDS,bd=NB_TENANT_DEFAULT_BURST_SECONDS;
        int invalid=parse_u64(tcp,100000,&a)||parse_u64(udp,100000,&b)||parse_u64(rate_up,100000000,&up);
        if(fields==6||fields==7){
            down=up;invalid=invalid||parse_u64(rate_down,UINT64_MAX/(1024ULL*1024ULL),&q);
            if(fields==7){invalid=invalid||parse_u64(quota,NB_TENANT_MAX_BURST_SECONDS,&bu)||bu==0;bd=bu;}
        }else if(!explicit_burst){
            invalid=invalid||parse_u64(rate_down,100000000,&down)||parse_u64(quota,UINT64_MAX/(1024ULL*1024ULL),&q)||
                parse_u64(burst_up,NB_TENANT_MAX_BURST_SECONDS,&bu)||parse_u64(burst_down,NB_TENANT_MAX_BURST_SECONDS,&bd)||bu==0||bd==0;
        }else{
            invalid=invalid||parse_u64(rate_down,100000000,&down)||parse_u64(quota,UINT64_MAX/(1024ULL*1024ULL),&q)||
                parse_u64(burst_up,UINT64_MAX,&bu)||parse_u64(burst_down,UINT64_MAX,&bd)||
                (up>0&&bu==0)||(down>0&&bd==0);
        }
        if(invalid){fclose(f);return fail(error,error_cap,"invalid tenant limits at line %u",line_no);}
        for(size_t i=0;i<next.count;i++)if(!strcmp(next.items[i].name,name)){fclose(f);return fail(error,error_cap,"duplicate tenant at line %u",line_no);}
        nb_tenant_t* t=&next.items[next.count++];snprintf(t->name,sizeof(t->name),"%s",name);t->max_tcp=(uint32_t)a;t->max_udp=(uint32_t)b;
        t->rate_bytes_per_sec[NB_TENANT_UP]=up?up*1000ULL/8ULL:0;t->rate_bytes_per_sec[NB_TENANT_DOWN]=down?down*1000ULL/8ULL:0;
        t->burst_bytes[NB_TENANT_UP]=explicit_burst?bu:t->rate_bytes_per_sec[NB_TENANT_UP]*bu;
        t->burst_bytes[NB_TENANT_DOWN]=explicit_burst?bd:t->rate_bytes_per_sec[NB_TENANT_DOWN]*bd;
        t->byte_quota=q*1024ULL*1024ULL;t->tokens[NB_TENANT_UP]=t->burst_bytes[NB_TENANT_UP];t->tokens[NB_TENANT_DOWN]=t->burst_bytes[NB_TENANT_DOWN];
    }
    fclose(f);if(next.count==0)return fail(error,error_cap,"tenant config is empty");
    next.config_fingerprint=nb_tenants_fingerprint(&next);*out=next;return 0;
}

int nb_tenants_enable_shared(nb_tenants_t* tenants,const char* path,char* error,size_t error_cap){return nb_tenant_shared_open(tenants,path,error,error_cap);}
void nb_tenants_close(nb_tenants_t* tenants){nb_tenant_shared_close(tenants);}
int nb_tenant_find(const nb_tenants_t* tenants,const char* name){if(!tenants||!name)return -1;for(size_t i=0;i<tenants->count;i++)if(!strcmp(tenants->items[i].name,name))return (int)i;return -1;}
int nb_tenants_same_identity(const nb_tenants_t* current,const nb_tenants_t* next){
    if(!current||!next||current->count!=next->count)return 0;
    for(size_t i=0;i<current->count;i++)if(strcmp(current->items[i].name,next->items[i].name))return 0;
    return 1;
}
int nb_tenants_reconfigure(nb_tenants_t* current,const nb_tenants_t* next,char* error,size_t error_cap){
    if(!nb_tenants_same_identity(current,next))return fail(error,error_cap,"tenant identity/order changed; full deployment required");
    if(current->shared_state&&nb_tenant_shared_reconfigure(current,next,error,error_cap)!=0)return -1;
    for(size_t i=0;i<current->count;i++){
        nb_tenant_t runtime=current->items[i];current->items[i]=next->items[i];
        current->items[i].active_tcp=runtime.active_tcp;current->items[i].active_udp=runtime.active_udp;
        current->items[i].bytes_up=runtime.bytes_up;current->items[i].bytes_down=runtime.bytes_down;
        current->items[i].rejected=runtime.rejected;
        for(int d=0;d<NB_TENANT_DIRECTIONS;d++){
            uint64_t cap=current->items[i].burst_bytes[d];
            current->items[i].tokens[d]=runtime.tokens[d]>cap?cap:runtime.tokens[d];
            current->items[i].token_updated_us[d]=runtime.token_updated_us[d];
            current->items[i].token_fraction[d]=runtime.token_fraction[d];
            current->items[i].media_active_until_us[d]=runtime.media_active_until_us[d];
        }
    }
    current->config_fingerprint=next->config_fingerprint;
    return 0;
}

int nb_tenant_acquire(nb_tenants_t* tenants,int index,int udp,uint64_t now_us){
    if(!tenants||index<0||(size_t)index>=tenants->count)return -1;
    if(tenants->shared_state)return nb_tenant_shared_acquire(tenants,index,udp,now_us);
    nb_tenant_t* t=&tenants->items[index];uint32_t* active=udp?&t->active_udp:&t->active_tcp;uint32_t limit=udp?t->max_udp:t->max_tcp;
    if((limit&&*active>=limit)||(t->byte_quota&&t->bytes_up+t->bytes_down>=t->byte_quota)){t->rejected++;return -1;}
    (*active)++;for(int d=0;d<NB_TENANT_DIRECTIONS;d++)if(t->token_updated_us[d]==0)t->token_updated_us[d]=now_us;return 0;
}

void nb_tenant_release(nb_tenants_t* tenants,int index,int udp){if(!tenants||index<0||(size_t)index>=tenants->count)return;if(tenants->shared_state){nb_tenant_shared_release(tenants,index,udp);return;}uint32_t* active=udp?&tenants->items[index].active_udp:&tenants->items[index].active_tcp;if(*active)(*active)--;}

size_t nb_tenant_allowance(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us,int direction){
    if(!tenants||index<0||(size_t)index>=tenants->count)return requested;
    if(!valid_direction(direction))return 0;
    if(tenants->shared_state)return nb_tenant_shared_allowance(tenants,index,requested,now_us,0,direction,0);
    nb_tenant_t* t=&tenants->items[index];if(t->byte_quota){uint64_t used=t->bytes_up+t->bytes_down;if(used>=t->byte_quota)return 0;uint64_t left=t->byte_quota-used;if(left<requested)requested=(size_t)left;}
    uint64_t rate=t->rate_bytes_per_sec[direction],burst=t->burst_bytes[direction];if(!rate)return requested;
    nb_tenant_refill(rate,burst,now_us,&t->tokens[direction],&t->token_updated_us[direction],
        &t->token_fraction[direction]);
    if(t->tokens[direction]<requested)requested=(size_t)t->tokens[direction];
    return requested;
}

void nb_tenant_consume(nb_tenants_t* tenants,int index,size_t bytes,int direction){if(!tenants||index<0||(size_t)index>=tenants->count||tenants->shared_state||!valid_direction(direction))return;nb_tenant_t* t=&tenants->items[index];t->tokens[direction]=t->tokens[direction]>bytes?t->tokens[direction]-bytes:0;}
size_t nb_tenant_take_class(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us,
    int direction,int media){
    if(!tenants||index<0||(size_t)index>=tenants->count)return requested;
    if(!valid_direction(direction))return 0;
    if(tenants->shared_state)return nb_tenant_shared_allowance(tenants,index,requested,now_us,1,direction,media);
    nb_tenant_t* tenant=&tenants->items[index];
    if(media&&requested>0)tenant->media_active_until_us[direction]=now_us+NB_TENANT_MEDIA_ACTIVE_US;
    size_t allowed=nb_tenant_allowance(tenants,index,requested,now_us,direction);
    if(!media&&now_us<tenant->media_active_until_us[direction]){
        uint64_t reserve=media_reserve_bytes(tenant,direction);
        uint64_t available=tenant->tokens[direction]>reserve?tenant->tokens[direction]-reserve:0;
        if((uint64_t)allowed>available)allowed=(size_t)available;
    }
    nb_tenant_consume(tenants,index,allowed,direction);return allowed;
}
size_t nb_tenant_take(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us,int direction){
    return nb_tenant_take_class(tenants,index,requested,now_us,direction,0);
}
uint64_t nb_tenant_retry_after_us(const nb_tenants_t* tenants,int index,size_t bytes,int direction){
    if(!tenants||index<0||(size_t)index>=tenants->count||!valid_direction(direction))return 0;
    uint64_t rate=tenants->items[index].rate_bytes_per_sec[direction];if(!rate)return 0;
    uint64_t needed=bytes?bytes:1,delay=(needed*1000000ULL+rate-1)/rate;
    return delay<1000ULL?1000ULL:delay;
}
void nb_tenant_refund(nb_tenants_t* tenants,int index,size_t bytes,int direction){if(!tenants||index<0||(size_t)index>=tenants->count||bytes==0||!valid_direction(direction))return;if(tenants->shared_state){nb_tenant_shared_refund(tenants,index,bytes,direction);return;}nb_tenant_t* t=&tenants->items[index];uint64_t burst=t->burst_bytes[direction];if(burst)t->tokens[direction]=bytes>=burst||t->tokens[direction]>=burst-bytes?burst:t->tokens[direction]+bytes;}

int64_t nb_tenant_wake_delay(uint64_t throttled_until_us,uint64_t now_us,int64_t current_delay_us){if(throttled_until_us==0)return current_delay_us;uint64_t raw=throttled_until_us<=now_us?0:throttled_until_us-now_us;int64_t delay=raw>(uint64_t)INT64_MAX?INT64_MAX:(int64_t)raw;return current_delay_us<0||delay<current_delay_us?delay:current_delay_us;}
void nb_tenant_account(nb_tenants_t* tenants,int index,uint64_t up,uint64_t down){if(!tenants||index<0||(size_t)index>=tenants->count)return;if(tenants->shared_state){nb_tenant_shared_account(tenants,index,up,down);return;}tenants->items[index].bytes_up+=up;tenants->items[index].bytes_down+=down;}

int nb_tenants_render_json(const nb_tenants_t* tenants,char* out,size_t cap){
    if(!tenants||!out||cap==0)return -1;
    if(tenants->shared_state&&nb_tenant_shared_snapshot((nb_tenants_t*)tenants)!=0)return -1;
    size_t off=(size_t)snprintf(out,cap,"{\"tenants\":[");
    for(size_t i=0;i<tenants->count&&off<cap;i++){
        const nb_tenant_t* t=&tenants->items[i];
        uint64_t up=t->rate_bytes_per_sec[NB_TENANT_UP]*8ULL/1000ULL;
        uint64_t down=t->rate_bytes_per_sec[NB_TENANT_DOWN]*8ULL/1000ULL;
        uint64_t compatible=up>down?up:down;
        int n=snprintf(out+off,cap-off,"%s{\"name\":\"%s\",\"active_tcp\":%u,\"active_udp\":%u,\"bytes_up\":%llu,\"bytes_down\":%llu,\"rate_kbps\":%llu,\"rate_up_kbps\":%llu,\"rate_down_kbps\":%llu,\"burst_up_bytes\":%llu,\"burst_down_bytes\":%llu,\"rejected\":%llu}",i?",":"",t->name,t->active_tcp,t->active_udp,(unsigned long long)t->bytes_up,(unsigned long long)t->bytes_down,(unsigned long long)compatible,(unsigned long long)up,(unsigned long long)down,(unsigned long long)t->burst_bytes[NB_TENANT_UP],(unsigned long long)t->burst_bytes[NB_TENANT_DOWN],(unsigned long long)t->rejected);
        if(n<0||(size_t)n>=cap-off)return -1;
        off+=(size_t)n;
    }
    int n=snprintf(out+off,cap-off,"]}\n");return n<0||(size_t)n>=cap-off?-1:(int)(off+(size_t)n);
}
