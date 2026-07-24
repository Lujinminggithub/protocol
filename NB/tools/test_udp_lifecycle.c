#include "nb_udp_lifecycle.h"

#include <stdio.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

int main(void){
    nb_udp_lifecycle_t state;nb_udp_lifecycle_init(&state);const uint8_t* wire=NULL;
    size_t wire_len=0,token=0;nb_udp_wire_view_t view;
    CHECK(nb_udp_lifecycle_schedule(&state,11,NB_UDP_TYPE_CLOSE,77,"T:127.0.0.1:50001",100)==0);
    CHECK(nb_udp_lifecycle_count(&state)==1&&nb_udp_lifecycle_mark_due(&state,99)==0);
    CHECK(nb_udp_lifecycle_mark_due(&state,100)==11);
    CHECK(nb_udp_lifecycle_next(&state,11,100,&wire,&wire_len,&token)==1);
    CHECK(nb_udp_wire_decode(wire,wire_len,&view)==0&&view.type==NB_UDP_TYPE_CLOSE&&view.session_id==77);
    nb_udp_lifecycle_sent(&state,token,100);CHECK(nb_udp_lifecycle_next_deadline(&state)==100+NB_UDP_CLOSE_RETRY_US);
    CHECK(nb_udp_lifecycle_next(&state,11,100+NB_UDP_CLOSE_RETRY_US,&wire,&wire_len,&token)==1);
    CHECK(nb_udp_lifecycle_ack(&state,11,77)==1&&nb_udp_lifecycle_count(&state)==0);
    CHECK(nb_udp_lifecycle_schedule(&state,12,NB_UDP_TYPE_CLOSE_ACK,88,"T:127.0.0.1:50001",200)==0);
    CHECK(nb_udp_lifecycle_next(&state,12,200,&wire,&wire_len,&token)==1);
    nb_udp_lifecycle_sent(&state,token,200);CHECK(nb_udp_lifecycle_count(&state)==0);
    CHECK(nb_udp_lifecycle_schedule(&state,13,NB_UDP_TYPE_CLOSE,99,"T:127.0.0.1:50001",300)==0);
    nb_udp_lifecycle_forget_link(&state,13);CHECK(nb_udp_lifecycle_count(&state)==0);
    puts("nb_udp_lifecycle_test: ok");return 0;
}
