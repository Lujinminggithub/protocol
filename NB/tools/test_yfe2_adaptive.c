#include "nb_yfe2_adaptive.h"

#include <stdio.h>

#define CHECK(value) do { if(!(value)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1;} } while(0)

static nb_yfe2_loss_sample_t baseline(void){return (nb_yfe2_loss_sample_t){.generation=7,.sent=1000};}

int main(void){
    nb_yfe2_adaptive_t state;nb_yfe2_adaptive_init(&state);nb_yfe2_loss_sample_t sample=baseline();
    CHECK(nb_yfe2_adaptive_update(&state,&sample,1000000)==NB_YFE2_MODE_BASELINE);
    sample.sent+=100;sample.declared_lost++;
    CHECK(nb_yfe2_adaptive_update(&state,&sample,1200000)==NB_YFE2_MODE_BURST);
    CHECK(state.burst_until_us==6200000&&state.effective_physical_loss==1);
    CHECK(nb_yfe2_adaptive_update(&state,&sample,6199999)==NB_YFE2_MODE_BURST);
    CHECK(nb_yfe2_adaptive_update(&state,&sample,6200000)==NB_YFE2_MODE_BASELINE);

    const uint32_t reasons[]={NB_YFE2_IGNORE_LOCAL_DROP,NB_YFE2_IGNORE_SCHEDULER,
        NB_YFE2_IGNORE_SEND_QUEUE,NB_YFE2_IGNORE_PMTU,NB_YFE2_IGNORE_APP_LIMITED,
        NB_YFE2_IGNORE_RATE_CAP,NB_YFE2_IGNORE_PROBE};
    for(size_t i=0;i<sizeof(reasons)/sizeof(reasons[0]);i++){
        nb_yfe2_adaptive_init(&state);sample=baseline();
        CHECK(nb_yfe2_adaptive_update(&state,&sample,1000000)==NB_YFE2_MODE_BASELINE);
        sample.sent+=reasons[i]==NB_YFE2_IGNORE_APP_LIMITED?1:100;
        sample.declared_lost++;sample.invalid_reasons=reasons[i];
        CHECK(nb_yfe2_adaptive_update(&state,&sample,1200000)==NB_YFE2_MODE_BASELINE);
        CHECK(state.effective_physical_loss==0&&state.ignored_samples[i]==1);
    }
    nb_yfe2_adaptive_init(&state);sample=baseline();
    CHECK(nb_yfe2_adaptive_update(&state,&sample,1000000)==NB_YFE2_MODE_BASELINE);
    sample.sent+=NB_YFE2_APP_LIMITED_MIN_PACKETS;sample.declared_lost++;
    sample.invalid_reasons=NB_YFE2_IGNORE_APP_LIMITED;
    CHECK(nb_yfe2_adaptive_update(&state,&sample,1200000)==NB_YFE2_MODE_BURST);
    CHECK(state.effective_physical_loss==1&&state.ignored_samples[4]==0);
    nb_yfe2_adaptive_init(&state);sample=baseline();nb_yfe2_adaptive_update(&state,&sample,1000000);
    sample.sent+=100;sample.declared_lost++;sample.spurious_lost++;
    CHECK(nb_yfe2_adaptive_update(&state,&sample,1200000)==NB_YFE2_MODE_BASELINE);
    sample.generation=8;sample.sent=1;sample.declared_lost=0;sample.spurious_lost=0;
    CHECK(nb_yfe2_adaptive_update(&state,&sample,1400000)==NB_YFE2_MODE_BASELINE);
    CHECK(state.effective_physical_loss==0);
    puts("nb_yfe2_adaptive_test: ok");return 0;
}
