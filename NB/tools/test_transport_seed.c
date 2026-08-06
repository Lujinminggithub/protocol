#include "nb_transport_seed.h"

#include <assert.h>
#include <string.h>

int main(void){
    nb_transport_link_profile_t link={0};
    strcpy(link.cc,"bbr");
    link.target_rate_bps=5000000;
    link.seed_rtt_us=202757;
    link.startup_cwin_bytes=262144;

    nb_transport_seed_plan_t plan={0};
    assert(nb_transport_seed_plan(&link,281000,&plan)==0);
    assert(plan.configured==1);
    assert(plan.rtt_us==281000);
    assert(plan.cwin_bytes==393216);

    assert(nb_transport_seed_plan(&link,0,&plan)==0);
    assert(plan.rtt_us==202757);
    assert(plan.cwin_bytes==262144);

    assert(nb_transport_seed_plan(&link,999,&plan)==0);
    assert(plan.rtt_us==202757);
    assert(plan.cwin_bytes==262144);

    strcpy(link.cc,"cubic");
    assert(nb_transport_seed_plan(&link,281000,&plan)==0);
    assert(plan.configured==0);
    return 0;
}
