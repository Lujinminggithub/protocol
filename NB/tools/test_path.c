#include "nb_path.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    nb_path_quality_t a={1.0,200000,10000,20,1000,0};
    nb_path_quality_t b={2.0,100000,5000,20,1000,1};
    assert(nb_path_quality_fresh(&a,2000,3000));assert(nb_path_quality_sampled(&a,2000,3000,20));
    assert(nb_path_quality_better(&a,&b,2000,3000,20,2.0));
    a.effective_loss_pct=b.effective_loss_pct;assert(nb_path_quality_better(&b,&a,2000,3000,20,2.0));
    assert(nb_path_loss_score(&(nb_path_quality_t){4.0,0,0,10,1,0},20,2.0)==3.0);
    assert(nb_path_sanitize_reorder_delay(123456)==123456);
    assert(nb_path_sanitize_reorder_delay(UINT64_MAX)==0);
    assert(nb_path_sanitize_reorder_gap(64)==64);
    assert(nb_path_sanitize_reorder_gap(UINT64_MAX)==0);
    puts("RESULT PASS");return 0;
}
