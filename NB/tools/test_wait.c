#include "nb_wait.h"

#include <assert.h>

int main(void){
    struct timespec timeout={0};
    assert(nb_wait_timespec(0,&timeout)==0);
    assert(timeout.tv_sec==0&&timeout.tv_nsec==0);
    assert(nb_wait_timespec(999,&timeout)==0);
    assert(timeout.tv_sec==0&&timeout.tv_nsec==999000);
    assert(nb_wait_timespec(1500,&timeout)==0);
    assert(timeout.tv_sec==0&&timeout.tv_nsec==1500000);
    assert(nb_wait_timespec(1000000,&timeout)==0);
    assert(timeout.tv_sec==1&&timeout.tv_nsec==0);

    assert(nb_wait_fallback_timeout_ms(0)==0);
    assert(nb_wait_fallback_timeout_ms(999)==0);
    assert(nb_wait_fallback_timeout_ms(1000)==1);
    assert(nb_wait_fallback_timeout_ms(1500)==1);
    assert(nb_wait_fallback_timeout_ms(1000001)==1000);
    return 0;
}
