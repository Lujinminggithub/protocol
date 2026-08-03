#include "nb_probe.h"
#include "nb_session.h"

#include <stdio.h>

int main(void){
    size_t stream_size=sizeof(proxy_stream_t);
    if(sizeof(((proxy_stream_t*)0)->queue_clocks)!=sizeof(void*))return 1;
    if(sizeof(((proxy_stream_t*)0)->logical_decoder)!=sizeof(void*))return 2;
    if(sizeof(((proxy_stream_t*)0)->udp_reassembly)!=sizeof(void*))return 3;
    if(sizeof(((proxy_stream_t*)0)->fec_ctrl_rxbuf)!=sizeof(void*))return 4;
    if(stream_size>8192u)return 5;
    printf("RESULT PASS proxy_stream=%zu decoder=%zu udp_reassembly=%zu\n",
        stream_size,sizeof(nb_lstream_decoder_t),sizeof(nb_udp_reassembly_t));
    return 0;
}
