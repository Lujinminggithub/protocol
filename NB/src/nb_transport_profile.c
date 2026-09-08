#define _POSIX_C_SOURCE 200809L
#include "nb_transport_profile.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fail(char* error,size_t cap,const char* format,...){
    if(error&&cap){va_list args;va_start(args,format);vsnprintf(error,cap,format,args);va_end(args);}return -1;
}

static int safe_id(const char* value,size_t limit){
    size_t length=value?strlen(value):0;if(length==0||length>limit)return 0;
    for(size_t i=0;i<length;i++)if(!(isalnum((unsigned char)value[i])||strchr("._-",value[i])))return 0;
    return 1;
}

static char* trim(char* value){
    while(*value==' '||*value=='\t')value++;
    size_t length=strlen(value);while(length&&strchr(" \t\r\n",value[length-1]))value[--length]=0;return value;
}

static uint64_t hash_bytes(const unsigned char* data,size_t length){
    uint64_t hash=14695981039346656037ULL;
    for(size_t i=0;i<length;i++){hash^=data[i];hash*=1099511628211ULL;}return hash;
}

static int parse_u64(const char* value,uint64_t minimum,uint64_t maximum,uint64_t* out){
    if(value==NULL||*value==0||*value=='-')return -1;
    char* end=NULL;errno=0;unsigned long long parsed=strtoull(value,&end,10);
    if(errno||end==value||*end||parsed<minimum||parsed>maximum)return -1;
    *out=(uint64_t)parsed;return 0;
}

static int parse_bool(const char* value,int* out){
    if(!strcmp(value,"true")||!strcmp(value,"1")){*out=1;return 0;}
    if(!strcmp(value,"false")||!strcmp(value,"0")){*out=0;return 0;}return -1;
}

static uint64_t directive_bit(const char* key){
    if(!strcmp(key,"schema"))return 1ULL<<0;
    if(!strcmp(key,"line_id"))return 1ULL<<1;
    if(!strcmp(key,"generation"))return 1ULL<<2;
    if(!strcmp(key,"role"))return 1ULL<<3;
    static const char* fields[]={"cc","bbr_options","cwin_max_bytes","mtu_max","reorder_gap",
        "reorder_delay_us","udp_gso","fec_observe","fec_active","udp_fec_adaptive","udp_fec_k",
        "udp_fec_hold_us","target_rate_bps","seed_rtt_us","startup_cwin_bytes","udp_fec_mode"};
    const char* field=NULL;unsigned offset=0;
    if(!strncmp(key,"ingress.",8)){field=key+8;offset=4;}
    else if(!strncmp(key,"egress.",7)){field=key+7;offset=20;}
    if(field)for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++)if(!strcmp(field,fields[i]))return 1ULL<<(offset+i);
    return 0;
}

static int parse_link(nb_transport_link_profile_t* link,const char* field,const char* value){
    link->present=1;
    if(!strcmp(field,"cc")){
        if(strcmp(value,"cubic")&&strcmp(value,"bbr")&&strcmp(value,"dcubic")&&strcmp(value,"fastcc"))return -1;
        if(strlen(value)>=sizeof(link->cc))return -1;
        memcpy(link->cc,value,strlen(value)+1);return 0;
    }
    if(!strcmp(field,"bbr_options")){
        if(strlen(value)>=sizeof(link->bbr_options))return -1;
        memcpy(link->bbr_options,value,strlen(value)+1);return 0;
    }
    if(!strcmp(field,"cwin_max_bytes"))return parse_u64(value,0,67108864ULL,&link->cwin_max_bytes)==0&&
        (link->cwin_max_bytes==0||link->cwin_max_bytes>=65536ULL)?0:-1;
    if(!strcmp(field,"mtu_max")){uint64_t parsed=0;if(parse_u64(value,1280,1536,&parsed))return -1;link->mtu_max=(uint32_t)parsed;return 0;}
    if(!strcmp(field,"reorder_gap")){uint64_t parsed=0;if(parse_u64(value,1,1024,&parsed))return -1;link->reorder_gap=(uint32_t)parsed;return 0;}
    if(!strcmp(field,"reorder_delay_us"))return parse_u64(value,1000,1000000,&link->reorder_delay_us);
    if(!strcmp(field,"udp_gso"))return parse_bool(value,&link->udp_gso);
    if(!strcmp(field,"fec_observe"))return parse_bool(value,&link->fec_observe);
    if(!strcmp(field,"fec_active"))return parse_bool(value,&link->fec_active);
    if(!strcmp(field,"udp_fec_adaptive"))return parse_bool(value,&link->udp_fec_adaptive);
    if(!strcmp(field,"udp_fec_k")){uint64_t parsed=0;if(parse_u64(value,2,8,&parsed))return -1;link->udp_fec_k=(uint32_t)parsed;return 0;}
    if(!strcmp(field,"udp_fec_hold_us"))return parse_u64(value,100,1000000,&link->udp_fec_hold_us);
    if(!strcmp(field,"udp_fec_mode")){
        if(strcmp(value,"off")&&strcmp(value,"adaptive-v2")&&strcmp(value,"nb-yfe2-optional"))return -1;
        memcpy(link->udp_fec_mode,value,strlen(value)+1);return 0;
    }
    if(!strcmp(field,"target_rate_bps"))return parse_u64(value,1000000,1000000000ULL,&link->target_rate_bps);
    if(!strcmp(field,"seed_rtt_us"))return parse_u64(value,1000,60000000ULL,&link->seed_rtt_us);
    if(!strcmp(field,"startup_cwin_bytes"))return parse_u64(value,65536,67108864ULL,&link->startup_cwin_bytes);
    return -1;
}

static int validate_link(const nb_transport_link_profile_t* link){
    if(!link->present)return 0;
    if(link->cc[0]==0||link->mtu_max<1280||link->reorder_gap<1||link->reorder_delay_us<1000)return -1;
    if(!strcmp(link->cc,"cubic")&&link->cwin_max_bytes<65536)return -1;
    if(link->fec_active&&!link->fec_observe)return -1;
    if(link->udp_fec_adaptive&&(link->udp_fec_k<2||link->udp_fec_k>8||
        link->udp_fec_hold_us<100||link->udp_fec_hold_us>1000000))return -1;
    int seed_fields=(link->target_rate_bps!=0)+(link->seed_rtt_us!=0)+(link->startup_cwin_bytes!=0);
    if(seed_fields!=0&&seed_fields!=3)return -1;
    return 0;
}

void nb_transport_profile_state_init(nb_transport_profile_state_t* state){if(state)memset(state,0,sizeof(*state));}

uint64_t nb_transport_shared_cwin_max(const nb_transport_link_profile_t* ingress,
    const nb_transport_link_profile_t* egress){
    if(ingress==NULL||!ingress->present)return egress&&egress->present?egress->cwin_max_bytes:0;
    if(egress==NULL||!egress->present)return ingress->cwin_max_bytes;
    if(ingress->cwin_max_bytes==0||egress->cwin_max_bytes==0)return 0;
    return ingress->cwin_max_bytes>egress->cwin_max_bytes?
        ingress->cwin_max_bytes:egress->cwin_max_bytes;
}

static int absolute_path(const char* path){
#ifdef _WIN32
    return path&&isalpha((unsigned char)path[0])&&path[1]==':'&&(path[2]=='/'||path[2]=='\\');
#else
    return path&&path[0]=='/';
#endif
}

int nb_transport_profile_load(const char* path,nb_transport_profile_t* profile,char* error,size_t error_cap){
    if(!absolute_path(path)||profile==NULL)return fail(error,error_cap,"invalid transport profile path");
    struct stat status;if(stat(path,&status)||!S_ISREG(status.st_mode))return fail(error,error_cap,"profile is not a regular file");
#ifndef _WIN32
    if(status.st_mode&(S_IWGRP|S_IWOTH))return fail(error,error_cap,"profile must not be group/world writable");
#endif
    FILE* file=fopen(path,"rb");if(file==NULL)return fail(error,error_cap,"open profile: %s",strerror(errno));
    if(fseek(file,0,SEEK_END)){fclose(file);return fail(error,error_cap,"seek profile failed");}
    long length=ftell(file);if(length<1||length>65536||fseek(file,0,SEEK_SET)){fclose(file);return fail(error,error_cap,"profile size is invalid");}
    unsigned char* bytes=malloc((size_t)length+1);if(bytes==NULL){fclose(file);return fail(error,error_cap,"profile allocation failed");}
    if(fread(bytes,1,(size_t)length,file)!=(size_t)length){free(bytes);fclose(file);return fail(error,error_cap,"read profile failed");}
    fclose(file);bytes[length]=0;memset(profile,0,sizeof(*profile));profile->fingerprint=hash_bytes(bytes,(size_t)length);
    uint64_t directives=0;char* save=NULL;for(char* line=strtok_r((char*)bytes,"\n",&save);line;line=strtok_r(NULL,"\n",&save)){
        line=trim(line);if(*line==0||*line=='#')continue;char* separator=strchr(line,'=');
        if(separator==NULL){free(bytes);return fail(error,error_cap,"profile directive is missing '='");}
        *separator=0;char* key=trim(line);char* value=trim(separator+1);uint64_t parsed=0;
        uint64_t bit=directive_bit(key);if(bit==0){free(bytes);return fail(error,error_cap,"unknown profile directive: %s",key);}
        if(directives&bit){free(bytes);return fail(error,error_cap,"duplicate profile directive: %s",key);}directives|=bit;
        if(!strcmp(key,"schema")){if(parse_u64(value,1,2,&parsed)){free(bytes);return fail(error,error_cap,"invalid profile schema");}profile->schema_version=(uint32_t)parsed;continue;}
        if(!strcmp(key,"line_id")){if(!safe_id(value,64)){free(bytes);return fail(error,error_cap,"invalid profile line id");}memcpy(profile->line_id,value,strlen(value)+1);continue;}
        if(!strcmp(key,"generation")){if(parse_u64(value,1,UINT64_MAX,&profile->generation)){free(bytes);return fail(error,error_cap,"invalid profile generation");}continue;}
        if(!strcmp(key,"role")){if(strcmp(value,"entry")&&strcmp(value,"middle")&&strcmp(value,"exit")){free(bytes);return fail(error,error_cap,"invalid profile role");}memcpy(profile->role,value,strlen(value)+1);continue;}
        nb_transport_link_profile_t* link=NULL;const char* field=NULL;
        if(!strncmp(key,"ingress.",8)){link=&profile->ingress;field=key+8;}
        else if(!strncmp(key,"egress.",7)){link=&profile->egress;field=key+7;}
        if(link==NULL||parse_link(link,field,value)){free(bytes);return fail(error,error_cap,"invalid profile directive: %s",key);}
    }
    free(bytes);
    if((profile->schema_version!=1&&profile->schema_version!=2)||profile->line_id[0]==0||profile->generation==0||profile->role[0]==0||
        validate_link(&profile->ingress)||validate_link(&profile->egress))return fail(error,error_cap,"profile is incomplete");
    if((!strcmp(profile->role,"entry")&&(!profile->egress.present||profile->ingress.present))||
        (!strcmp(profile->role,"middle")&&(!profile->ingress.present||!profile->egress.present))||
        (!strcmp(profile->role,"exit")&&(!profile->ingress.present||profile->egress.present)))return fail(error,error_cap,"profile links do not match role");
    if(profile->schema_version==1&&(profile->ingress.udp_fec_mode[0]||profile->egress.udp_fec_mode[0]))
        return fail(error,error_cap,"schema 1 does not support udp_fec_mode");
    if(profile->schema_version==2){
        const char* ingress=profile->ingress.udp_fec_mode[0]?profile->ingress.udp_fec_mode:"off";
        const char* egress=profile->egress.udp_fec_mode[0]?profile->egress.udp_fec_mode:"off";
        if(!strcmp(ingress,"nb-yfe2-optional")||
            (!strcmp(egress,"nb-yfe2-optional")&&strcmp(profile->role,"middle")))
            return fail(error,error_cap,"nb-yfe2-optional is only valid on middle egress");
    }
    return 0;
}

int nb_transport_profile_prepare(nb_transport_profile_state_t* state,const char* path,uint64_t generation,
    uint64_t fingerprint,const char* line_id,const char* role,char* error,size_t error_cap){
    if(state==NULL||!safe_id(line_id,64)||role==NULL)return fail(error,error_cap,"invalid profile prepare request");
    nb_transport_profile_t loaded;if(nb_transport_profile_load(path,&loaded,error,error_cap))return -1;
    if(loaded.generation!=generation||loaded.fingerprint!=fingerprint||strcmp(loaded.line_id,line_id)||strcmp(loaded.role,role))
        return fail(error,error_cap,"profile identity or fingerprint mismatch");
    if(state->has_active&&generation<state->active.generation)return fail(error,error_cap,"profile generation is stale");
    if(state->has_active&&generation==state->active.generation){
        if(fingerprint!=state->active.fingerprint)return fail(error,error_cap,"active generation fingerprint mismatch");
        return 0;
    }
    state->pending=loaded;state->has_pending=1;return 0;
}

int nb_transport_profile_commit(nb_transport_profile_state_t* state,uint64_t generation,char* error,size_t error_cap){
    if(state&&state->has_active&&state->active.generation==generation&&!state->has_pending)return 0;
    if(state==NULL||!state->has_pending||state->pending.generation!=generation)return fail(error,error_cap,"prepared profile generation is unavailable");
    if(state->has_active){state->previous=state->active;state->has_previous=1;}
    state->active=state->pending;state->has_active=1;memset(&state->pending,0,sizeof(state->pending));state->has_pending=0;return 0;
}

int nb_transport_profile_abort(nb_transport_profile_state_t* state,uint64_t generation){
    if(state==NULL)return -1;
    if(state->has_pending&&state->pending.generation==generation){memset(&state->pending,0,sizeof(state->pending));state->has_pending=0;}
    return 0;
}

int nb_transport_profile_rollback(nb_transport_profile_state_t* state,uint64_t generation,char* error,size_t error_cap){
    if(state==NULL||!state->has_previous||state->previous.generation!=generation)return fail(error,error_cap,"rollback generation is unavailable");
    nb_transport_profile_t current=state->active;state->active=state->previous;state->previous=current;state->has_active=1;state->has_previous=1;return 0;
}

int nb_transport_profile_render_status(const nb_transport_profile_state_t* state,char* out,size_t cap){
    if(state==NULL||out==NULL||cap==0)return -1;
    int n=snprintf(out,cap,"{\"status\":\"ok\",\"active_generation\":%llu,\"active_fingerprint\":\"%016llx\",\"pending_generation\":%llu,\"previous_generation\":%llu}\n",
        (unsigned long long)(state->has_active?state->active.generation:0),(unsigned long long)(state->has_active?state->active.fingerprint:0),
        (unsigned long long)(state->has_pending?state->pending.generation:0),(unsigned long long)(state->has_previous?state->previous.generation:0));
    return n<0||(size_t)n>=cap?-1:n;
}
