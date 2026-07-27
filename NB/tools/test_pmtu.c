#include <assert.h>
#include "nb_pmtu.h"

int main(void){
    nb_pmtu_state_t s;
    nb_pmtu_init(&s,1280,1500);assert(nb_pmtu_effective(&s)==1280);
    assert(nb_pmtu_observe(&s,1420,0,0,1)==0);
    assert(nb_pmtu_observe(&s,1420,0,0,2)==0);
    assert(nb_pmtu_observe(&s,1420,0,0,3)==1&&nb_pmtu_effective(&s)==1420);
    assert(nb_pmtu_observe(&s,1500,3,1,4)==0&&nb_pmtu_effective(&s)==1420);
    assert(nb_pmtu_observe(&s,1280,0,0,5)==-1&&nb_pmtu_effective(&s)==1280);
    assert(s.fallbacks==1&&s.promotions==1);
    assert(nb_pmtu_observe(&s,1500,0,0,100)==0);
    assert(nb_pmtu_observe(&s,1500,0,0,60000005)==0);
    assert(nb_pmtu_observe(&s,1500,0,0,60000006)==0);
    assert(nb_pmtu_observe(&s,1500,0,0,60000007)==1&&nb_pmtu_effective(&s)==1500);
    nb_pmtu_init(&s,900,800);assert(s.floor==1200&&s.ceiling==1200);
    return 0;
}
