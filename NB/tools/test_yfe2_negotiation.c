#include "nb_yfe2_negotiation.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){ \
    fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1; } } while(0)

int main(void){
    uint8_t wire[NB_YFE2_CONTROL_SIZE];nb_yfe2_control_t decoded;
    nb_yfe2_control_t proposal={0};proposal.type=NB_YFE2_CTL_PROPOSE;
    proposal.nonce=0x0102030405060708ULL;nb_yfe2_default_profile(&proposal.profile);
    CHECK(nb_yfe2_control_encode(wire,sizeof(wire),&proposal)==NB_YFE2_CONTROL_SIZE);
    CHECK(!memcmp(wire,"NBFC",4)&&wire[4]==1&&wire[5]==NB_YFE2_CTL_PROPOSE);
    CHECK(wire[6]==NB_YFE2_REASON_NONE&&wire[7]==0&&wire[38]==0&&wire[39]==0);
    CHECK(nb_yfe2_control_decode(wire,sizeof(wire),&decoded)==0);
    CHECK(decoded.nonce==proposal.nonce&&decoded.profile_id==28909);
    wire[7]=1;CHECK(nb_yfe2_control_decode(wire,sizeof(wire),&decoded)!=0);wire[7]=0;
    wire[5]=0xff;CHECK(nb_yfe2_control_decode(wire,sizeof(wire),&decoded)!=0);

    nb_yfe2_negotiation_t state;nb_yfe2_negotiation_init(&state);
    CHECK(nb_yfe2_negotiation_start(&state,proposal.nonce,1000000)==0);
    CHECK(state.state==NB_YFE2_NEG_PROPOSED);
    CHECK(nb_yfe2_negotiation_timeout(&state,1499999)==NB_YFE2_NEG_PROPOSED);
    CHECK(nb_yfe2_negotiation_timeout(&state,1500000)==NB_YFE2_NEG_FALLBACK_TIMEOUT);
    nb_yfe2_negotiation_init(&state);CHECK(nb_yfe2_negotiation_start(&state,proposal.nonce,2000000)==0);
    CHECK(nb_yfe2_negotiation_accept(&state,proposal.nonce+1,28909)!=0);
    CHECK(state.state==NB_YFE2_NEG_PROPOSED);
    CHECK(nb_yfe2_negotiation_accept(&state,proposal.nonce,28909)==0);
    CHECK(state.state==NB_YFE2_NEG_ACCEPTED);
    CHECK(!strcmp(nb_yfe2_negotiation_state_name(state.state),"accepted"));
    puts("nb_yfe2_negotiation_test: ok");return 0;
}
