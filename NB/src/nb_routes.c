#include "nb_routes.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* out,size_t cap,const char* fmt,...){if(out&&cap){va_list ap;va_start(ap,fmt);vsnprintf(out,cap,fmt,ap);va_end(ap);}return -1;}

int nb_routes_load(nb_routes_t* routes,const char* path,char* error,size_t error_cap){
    if(!routes||!path)return fail(error,error_cap,"missing route config");
    FILE* f=fopen(path,"r");if(!f)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    nb_routes_t next;memset(&next,0,sizeof(next));char line[512];unsigned line_no=0;
    while(fgets(line,sizeof(line),f)){
        line_no++;char* p=line;while(*p==' '||*p=='\t')p++;if(*p==0||*p=='#'||*p=='\r'||*p=='\n')continue;
        char keyword[16],name[32],hop[256],capacity_text[32]={0},extra[16];unsigned weight=0;
        int fields=sscanf(p,"%15s %31s %255s %u %31s %15s",keyword,name,hop,&weight,capacity_text,extra);
        if((fields!=4&&fields!=5)||strcmp(keyword,"route")||strncmp(hop,"H:",2)||weight==0||weight>1000){
            fclose(f);return fail(error,error_cap,"invalid route at line %u",line_no);
        }
        const char* colon=strrchr(hop+2,':');char* end=NULL;long port=colon?strtol(colon+1,&end,10):0;
        if(!colon||colon==hop+2||!end||*end||port<=0||port>65535||next.count>=NB_ROUTE_MAX||next.total_weight>UINT32_MAX-weight){
            fclose(f);return fail(error,error_cap,"invalid route endpoint at line %u",line_no);
        }
        for(size_t i=0;i<next.count;i++)if(!strcmp(next.entries[i].name,name)){fclose(f);return fail(error,error_cap,"duplicate route name at line %u",line_no);}
        char* cap_end=NULL;unsigned long capacity=fields==5?strtoul(capacity_text,&cap_end,10):0;
        if(fields==5&&(cap_end==capacity_text||*cap_end||capacity>100000)){fclose(f);return fail(error,error_cap,"invalid route capacity at line %u",line_no);}
        nb_route_entry_t* entry=&next.entries[next.count++];strcpy(entry->name,name);strcpy(entry->hop,hop);entry->weight=(uint16_t)weight;entry->capacity=(uint32_t)capacity;
        next.total_weight+=weight;
    }
    fclose(f);if(next.count==0)return fail(error,error_cap,"route config is empty");*routes=next;return 0;
}

uint64_t nb_routes_hash(const char* first,const char* second,uint16_t port){uint64_t h=1469598103934665603ULL;const char* parts[2]={first?first:"",second?second:""};for(int p=0;p<2;p++)for(const unsigned char* s=(const unsigned char*)parts[p];*s;s++){h^=*s;h*=1099511628211ULL;}h^=port;h*=1099511628211ULL;return h;}

int nb_routes_hop_endpoint(const nb_route_entry_t* route,char* host,size_t host_cap,uint16_t* port){
    if(!route||!host||host_cap==0||!port||strncmp(route->hop,"H:",2))return -1;
    const char* start=route->hop+2;const char* colon=strrchr(start,':');char* end=NULL;
    long parsed=colon?strtol(colon+1,&end,10):0;size_t length=colon?(size_t)(colon-start):0;
    if(!colon||length==0||length>=host_cap||!end||*end||parsed<1||parsed>65535)return -1;
    memcpy(host,start,length);host[length]=0;*port=(uint16_t)parsed;return 0;
}

int nb_routes_signal_direct_candidate(const char* rule_name,int udp_mode,int enabled){
    static const char* const rules[]={"live-netacc*","rtc-access*","frontier*","teko*"};
    if(!enabled||udp_mode||!rule_name)return 0;
    for(size_t i=0;i<sizeof(rules)/sizeof(rules[0]);i++)if(!strcmp(rule_name,rules[i]))return 1;
    return 0;
}

const nb_route_entry_t* nb_routes_pick_key(nb_routes_t* routes,uint64_t key,uint64_t now_us){
    if(!routes)return NULL;
    size_t best=NB_ROUTE_MAX;uint64_t best_score=0;
    for(size_t i=0;i<routes->count;i++){nb_route_entry_t* e=&routes->entries[i];if(e->unhealthy_until_us>now_us||(e->capacity&&e->active>=e->capacity)){e->rejected++;continue;}uint64_t x=key^nb_routes_hash(e->name,e->hop,(uint16_t)i);x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;uint64_t score=x/(1001ULL-e->weight);if(best==NB_ROUTE_MAX||score>best_score){best=i;best_score=score;}}
    return best==NB_ROUTE_MAX?NULL:&routes->entries[best];
}

int nb_routes_acquire(nb_routes_t* routes,const nb_route_entry_t* route){if(!routes||!route||route<routes->entries||route>=routes->entries+routes->count)return -1;nb_route_entry_t* e=&routes->entries[route-routes->entries];if(e->capacity&&e->active>=e->capacity){e->rejected++;return -1;}e->active++;e->selected++;return (int)(e-routes->entries);}
void nb_routes_release(nb_routes_t* routes,size_t index){if(routes&&index<routes->count&&routes->entries[index].active)routes->entries[index].active--;}
void nb_routes_finish_session(nb_routes_t* routes,size_t index){nb_routes_release(routes,index);}
void nb_routes_report_result(nb_routes_t* routes,size_t index,int result,uint64_t now_us,uint64_t cooldown_us){if(!routes||index>=routes->count||result==0)return;nb_route_entry_t* e=&routes->entries[index];if(result>0){e->failures=0;e->unhealthy_until_us=0;}else if(++e->failures>=3){e->unhealthy_until_us=now_us+cooldown_us;e->failures=0;}}
void nb_routes_report(nb_routes_t* routes,size_t index,int success,uint64_t now_us,uint64_t cooldown_us){nb_routes_report_result(routes,index,success?1:-1,now_us,cooldown_us);}
int nb_routes_report_name(nb_routes_t* routes,const char* name,int result,uint64_t now_us,uint64_t cooldown_us){
    if(!routes||!name||!*name||result<-1||result>1)return -1;
    for(size_t i=0;i<routes->count;i++)if(!strcmp(routes->entries[i].name,name)){
        nb_routes_report_result(routes,i,result,now_us,cooldown_us);return 0;
    }
    return -1;
}
int nb_routes_control_command(nb_routes_t* routes,const char* command,char* out,size_t cap,uint64_t now_us,uint64_t cooldown_us){
    if(!routes||!command||!out||!cap||strncmp(command,"route-result ",13))return -1;
    char name[32],result_text[16],extra[2];
    if(sscanf(command+13,"%31s %15s %1s",name,result_text,extra)!=2)
        return snprintf(out,cap,"{\"error\":\"invalid route-result\"}\n");
    int result=!strcmp(result_text,"success")?1:!strcmp(result_text,"failure")?-1:
        !strcmp(result_text,"neutral")?0:2;
    if(result==2||nb_routes_report_name(routes,name,result,now_us,cooldown_us)!=0)
        return snprintf(out,cap,"{\"error\":\"unknown route or result\"}\n");
    return snprintf(out,cap,"{\"status\":\"ok\",\"route\":\"%s\",\"result\":\"%s\"}\n",name,result_text);
}

int nb_routes_render_json(const nb_routes_t* routes,char* out,size_t cap,uint64_t now_us){if(!routes||!out||!cap)return -1;size_t off=(size_t)snprintf(out,cap,"{\"routes\":[");for(size_t i=0;i<routes->count;i++){const nb_route_entry_t* e=&routes->entries[i];int n=snprintf(out+off,cap-off,"%s{\"name\":\"%s\",\"active\":%u,\"capacity\":%u,\"healthy\":%s,\"selected\":%llu,\"rejected\":%llu}",i?",":"",e->name,e->active,e->capacity,e->unhealthy_until_us>now_us?"false":"true",(unsigned long long)e->selected,(unsigned long long)e->rejected);if(n<0||(size_t)n>=cap-off)return -1;off+=(size_t)n;}int n=snprintf(out+off,cap-off,"]}\n");return n<0||(size_t)n>=cap-off?-1:(int)(off+(size_t)n);}

const nb_route_entry_t* nb_routes_pick(nb_routes_t* routes){
    if(!routes||routes->count==0||routes->total_weight==0)return NULL;
    uint32_t point=routes->cursor++%routes->total_weight,base=0;
    for(size_t i=0;i<routes->count;i++){base+=routes->entries[i].weight;if(point<base)return &routes->entries[i];}
    return &routes->entries[routes->count-1];
}
