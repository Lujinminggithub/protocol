#include "nb_send.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
    nb_ring_t ring={0};nb_live_queue_clock_t clock={0};size_t peak=0;char out[8]={0};
    assert(nb_send_append(&ring,&clock,(const uint8_t*)"abc",3,16,1000,&peak)==0&&peak==3);
    assert(nb_send_append(&ring,&clock,(const uint8_t*)"def",3,16,8000,&peak)==0&&peak==6);
    assert(nb_live_queue_age_us(&clock,ring.len,11000)==10000);
    assert(nb_send_copyout(&ring,&clock,(uint8_t*)out,3)==3&&!memcmp(out,"abc",3));
    assert(nb_live_queue_age_us(&clock,ring.len,11000)==3000);
    assert(!nb_send_local_write_pending(0,0,0));
    assert(nb_send_local_write_pending(1,0,0));
    assert(nb_send_local_write_pending(0,1,0));
    assert(nb_send_local_write_pending(0,0,1));
    nb_send_clear(&ring,&clock);assert(ring.len==0&&clock.count==0);nb_ring_dispose(&ring);
    puts("RESULT PASS");return 0;
}
