#include "nb_bridge.h"

#include <stdlib.h>
#include <string.h>

const char* nb_bridge_role_name(int role){return role==0?"entry":(role==1?"middle":"exit");}

int nb_bridge_parse_port(const char* text,int* port){
    if(text==NULL||port==NULL)return -1;
    char* end=NULL;long value=strtol(text,&end,10);
    if(text==end||*end||value<=0||value>65535)return -1;
    *port=(int)value;return 0;
}

static int parse_target_at(const char* start,char* host,size_t host_cap,int* port){
    char tmp[300];const char* comma=strchr(start,',');size_t len=comma?(size_t)(comma-start):strlen(start);
    if(host==NULL||host_cap==0||port==NULL||len==0||len>=sizeof(tmp))return -1;
    memcpy(tmp,start,len);tmp[len]=0;char* colon=strrchr(tmp,':');if(colon==NULL)return -1;*colon=0;
    size_t host_len=strlen(tmp);if(host_len==0||host_len>=host_cap)return -1;
    memcpy(host,tmp,host_len+1);return nb_bridge_parse_port(colon+1,port);
}

int nb_bridge_parse_target(const char* route,char* host,size_t host_cap,int* port){
    if(route==NULL)return -1;
    const char* target=strstr(route,"T:");
    return target?parse_target_at(target+2,host,host_cap,port):-1;
}

int nb_bridge_parse_target_exact(const char* route,char* host,size_t host_cap,int* port){
    return route&&strncmp(route,"T:",2)==0?parse_target_at(route+2,host,host_cap,port):-1;
}

int nb_bridge_consume_hop(const char* route,char* next_host,size_t host_cap,int* next_port,
    char* rest,size_t rest_cap){
    if(route==NULL||next_host==NULL||host_cap==0||next_port==NULL||rest==NULL||rest_cap==0||strncmp(route,"H:",2)!=0)return -1;
    const char* comma=strchr(route,',');if(comma==NULL||comma[1]==0)return -1;
    size_t first_len=(size_t)(comma-route);if(first_len<4||first_len>=300||strlen(comma+1)>=rest_cap)return -1;
    char first[300];memcpy(first,route,first_len);first[first_len]=0;char* colon=strrchr(first+2,':');
    if(colon==NULL)return -1;
    *colon=0;
    if(first[2]==0||strlen(first+2)>=host_cap||nb_bridge_parse_port(colon+1,next_port)!=0)return -1;
    memcpy(next_host,first+2,strlen(first+2)+1);memcpy(rest,comma+1,strlen(comma+1)+1);return 0;
}
