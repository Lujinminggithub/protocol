#include "nb_fec_policy.h"

#include "nb_fec.h"

#include <stdio.h>
#include <string.h>

#define NB_FEC_ROBUST_LOSS_PCT 5.0
#define NB_FEC_ROBUST_JITTER_MS 60.0

static int params_valid(unsigned k, unsigned r){
    return k > 0 && k <= NB_FEC_MAX_K && r > 0 && r <= NB_FEC_MAX_R;
}

nb_fec_profile_t nb_fec_profile_select(double loss_pct, double jitter_ms,
    int coldstart, int robust_override)
{
    nb_fec_profile_t profile={NB_FEC_PROFILE_BALANCED,8,1};
    if(robust_override || (!coldstart && (loss_pct >= NB_FEC_ROBUST_LOSS_PCT ||
        jitter_ms >= NB_FEC_ROBUST_JITTER_MS))){
        profile.level=NB_FEC_PROFILE_ROBUST;
        profile.r=2;
    }
    return profile;
}

const char* nb_fec_profile_name(nb_fec_profile_level_t level){
    return level==NB_FEC_PROFILE_ROBUST?"robust":"balanced";
}

int nb_fec_start_render(char* out, size_t cap, uint32_t session_id, int priority,
    uint8_t k, uint8_t r, const char* route)
{
    if(out==NULL||cap==0||session_id==0||priority<0||priority>255||
        !params_valid(k,r)||route==NULL||route[0]==0||strchr(route,'\n')!=NULL)return -1;
    int written=snprintf(out,cap,"FC:START:%u:%d:%u:%u:%s\n",
        session_id,priority,k,r,route);
    return written>0&&(size_t)written<cap?written:-1;
}

int nb_fec_start_parse(const char* line, uint8_t legacy_k, uint8_t legacy_r,
    nb_fec_start_t* out)
{
    if(line==NULL||out==NULL||strncmp(line,"FC:START:",9)!=0)return -1;
    unsigned sid=0,k=0,r=0;int priority=0,offset=0;
    memset(out,0,sizeof(*out));
    if(sscanf(line,"FC:START:%u:%d:%u:%u:%n",&sid,&priority,&k,&r,&offset)!=4){
        k=legacy_k;r=legacy_r;offset=0;
        if(sscanf(line,"FC:START:%u:%d:%n",&sid,&priority,&offset)!=2)return -1;
    }
    if(sid==0||priority<0||priority>255||!params_valid(k,r)||offset<=0||
        line[offset]==0||line[offset]=='\n')return -1;
    size_t route_len=strcspn(line+offset,"\r\n");
    if(route_len==0||route_len>=sizeof(out->route)||line[offset+route_len+strspn(line+offset+route_len,"\r\n")]!=0)return -1;
    memcpy(out->route,line+offset,route_len);out->route[route_len]=0;
    out->session_id=(uint32_t)sid;out->priority=priority;
    out->k=(uint8_t)k;out->r=(uint8_t)r;
    return 0;
}
