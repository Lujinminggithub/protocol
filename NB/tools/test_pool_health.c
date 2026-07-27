#include "nb_pool_health.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    nb_pool_health_t state={0};
    nb_pool_health_config_t config={750000,5000000,12000000,30000000};
    assert(!nb_pool_health_update(&state,&config,1000000,1,800000,1,100));
    assert(!nb_pool_health_update(&state,&config,11999999,1,800000,1,100));
    assert(state.quarantined==1);
    assert(nb_pool_health_update(&state,&config,13000000,1,800000,1,100));
    assert(state.retire_count==1);
    assert(!nb_pool_health_update(&state,&config,14000000,1,800000,1,100));
    assert(!nb_pool_health_update(&state,&config,19000000,1,800000,1,100));
    assert(state.suppressed_count==1);
    assert(!nb_pool_health_update(&state,&config,20000000,1,100000,1,100));
    assert(state.degraded_since_us==0&&state.quarantined==0);
    assert(!nb_pool_health_update(&state,&config,44000000,1,800000,1,100));
    assert(nb_pool_health_update(&state,&config,49000000,1,800000,1,100));
    assert(state.retire_count==2);
    assert(!nb_pool_health_retire_now(&state,&config,50000000));
    assert(nb_pool_health_retire_now(&state,&config,80000000));
    assert(state.retire_count==3);
    /* ACK progress keeps a blocked connection alive; queue recovery clears it. */
    nb_pool_health_t spike={0};
    assert(!nb_pool_health_update(&spike,&config,1000000,1,1600000,1,100));
    assert(!nb_pool_health_update(&spike,&config,7000000,1,1600000,1,101));
    assert(!nb_pool_health_update(&spike,&config,18000000,1,1600000,1,101));
    assert(!nb_pool_health_update(&spike,&config,18500000,1,100000,0,101));
    assert(spike.retire_count==0&&spike.degraded_since_us==0);
    /* Recovery clears quarantine without requiring a destructive retirement. */
    nb_pool_health_t transient={0};
    assert(!nb_pool_health_update(&transient,&config,1000000,1,800000,1,100));
    assert(transient.quarantined==1);
    assert(!nb_pool_health_update(&transient,&config,7000000,1,100000,0,101));
    assert(transient.quarantined==0&&transient.retire_count==0);
    nb_pool_health_t recovery={.quarantined=1};
    assert(nb_pool_health_recovery_action(&recovery,0,0,0)==NB_POOL_RECOVERY_START_REPLACEMENT);
    assert(nb_pool_health_recovery_action(&recovery,1,0,0)==NB_POOL_RECOVERY_NONE);
    assert(nb_pool_health_recovery_action(&recovery,1,1,1)==NB_POOL_RECOVERY_NONE);
    assert(nb_pool_health_recovery_action(&recovery,1,1,0)==NB_POOL_RECOVERY_PROMOTE_REPLACEMENT);
    recovery.quarantined=0;
    assert(nb_pool_health_recovery_action(&recovery,1,1,0)==NB_POOL_RECOVERY_CLOSE_REPLACEMENT);
    puts("RESULT PASS");return 0;
}
