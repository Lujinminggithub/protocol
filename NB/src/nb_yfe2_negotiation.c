#include "nb_yfe2_negotiation.h"

#include <string.h>

static void put16(uint8_t* p,uint16_t value){p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value;}
static void put64(uint8_t* p,uint64_t value){for(int i=7;i>=0;i--){p[i]=(uint8_t)value;value>>=8;}}
static uint16_t get16(const uint8_t* p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint64_t get64(const uint8_t* p){uint64_t value=0;for(int i=0;i<8;i++)value=(value<<8)|p[i];return value;}

static int reason_valid(uint8_t type,uint8_t reason){
    if(type==NB_YFE2_CTL_PROPOSE||type==NB_YFE2_CTL_ACCEPT)return reason==NB_YFE2_REASON_NONE;
    return type==NB_YFE2_CTL_REJECT&&reason>=NB_YFE2_REASON_NOT_SUPPORTED&&reason<=NB_YFE2_REASON_RESOURCE;
}

int nb_yfe2_control_encode(uint8_t* out,size_t cap,const nb_yfe2_control_t* control){
    if(out==NULL||control==NULL||cap<NB_YFE2_CONTROL_SIZE||control->nonce==0||
        !reason_valid(control->type,control->reason)||nb_yfe2_profile_validate(&control->profile)!=0)return -1;
    uint16_t profile_id=nb_yfe2_profile_id(&control->profile);
    if((control->profile_id&&control->profile_id!=profile_id)||profile_id!=NB_YFE2_PROFILE_ID)return -1;
    memset(out,0,NB_YFE2_CONTROL_SIZE);memcpy(out,"NBFC",4);out[4]=1;out[5]=control->type;
    out[6]=control->reason;put64(out+8,control->nonce);put16(out+16,profile_id);
    out[18]=control->profile.wire_version;out[19]=1;out[20]=control->profile.data_shards;
    out[21]=control->profile.parity_shards;out[22]=control->profile.burst_parity_shards;
    out[23]=control->profile.interleave;put16(out+24,control->profile.flush_ms);
    put16(out+26,control->profile.recovery_deadline_ms);put64(out+28,control->profile.loss_trigger_packets);
    put16(out+36,control->profile.hold_ms);return (int)NB_YFE2_CONTROL_SIZE;
}

int nb_yfe2_control_decode(const uint8_t* wire,size_t length,nb_yfe2_control_t* out){
    if(wire==NULL||out==NULL||length!=NB_YFE2_CONTROL_SIZE||memcmp(wire,"NBFC",4)||wire[4]!=1||
        !reason_valid(wire[5],wire[6])||wire[7]||wire[19]!=1||wire[38]||wire[39])return -1;
    nb_yfe2_control_t value={0};value.type=wire[5];value.reason=wire[6];value.nonce=get64(wire+8);
    value.profile_id=get16(wire+16);value.profile=(nb_yfe2_profile_t){wire[18],wire[20],wire[21],
        wire[22],wire[23],get16(wire+24),get16(wire+26),get64(wire+28),get16(wire+36)};
    if(value.nonce==0||nb_yfe2_profile_validate(&value.profile)!=0||
        value.profile_id!=nb_yfe2_profile_id(&value.profile))return -1;
    *out=value;return 0;
}

void nb_yfe2_negotiation_init(nb_yfe2_negotiation_t* state){if(state)memset(state,0,sizeof(*state));}
int nb_yfe2_negotiation_start(nb_yfe2_negotiation_t* state,uint64_t nonce,uint64_t now_us){
    if(state==NULL||nonce==0||now_us>UINT64_MAX-NB_YFE2_NEGOTIATION_TIMEOUT_US)return -1;
    *state=(nb_yfe2_negotiation_t){NB_YFE2_NEG_PROPOSED,nonce,now_us+NB_YFE2_NEGOTIATION_TIMEOUT_US,NB_YFE2_PROFILE_ID};return 0;
}
nb_yfe2_negotiation_state_t nb_yfe2_negotiation_timeout(nb_yfe2_negotiation_t* state,uint64_t now_us){
    if(state&&state->state==NB_YFE2_NEG_PROPOSED&&now_us>=state->deadline_us)state->state=NB_YFE2_NEG_FALLBACK_TIMEOUT;
    return state?state->state:NB_YFE2_NEG_DISABLED;
}
int nb_yfe2_negotiation_accept(nb_yfe2_negotiation_t* state,uint64_t nonce,uint16_t profile_id){
    if(state==NULL||state->state!=NB_YFE2_NEG_PROPOSED||nonce!=state->nonce||profile_id!=state->profile_id)return -1;
    state->state=NB_YFE2_NEG_ACCEPTED;return 0;
}
int nb_yfe2_negotiation_reject(nb_yfe2_negotiation_t* state,uint64_t nonce,uint8_t reason){
    if(state==NULL||state->state!=NB_YFE2_NEG_PROPOSED||nonce!=state->nonce||reason<1||reason>4)return -1;
    state->state=(nb_yfe2_negotiation_state_t)(NB_YFE2_NEG_FALLBACK_NOT_SUPPORTED+reason-1);return 0;
}
const char* nb_yfe2_negotiation_state_name(nb_yfe2_negotiation_state_t state){
    static const char* names[]={"disabled","proposed","accepted","fallback_timeout","fallback_not_supported",
        "fallback_profile_mismatch","fallback_pmtu","fallback_resource"};
    return state>=NB_YFE2_NEG_DISABLED&&state<=NB_YFE2_NEG_FALLBACK_RESOURCE?names[state]:"invalid";
}
