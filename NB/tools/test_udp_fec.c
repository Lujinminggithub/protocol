#include "nb_udp_fec.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do{if(!(value)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1;}}while(0)

static int make_source(uint8_t* wire,size_t cap,uint32_t sequence,uint8_t fill,nb_udp_wire_view_t* view){
    uint8_t payload[900];memset(payload,fill,sizeof(payload));const char* route="T:live.example:443";
    int length=nb_udp_wire_encode(wire,cap,NB_UDP_TYPE_C2S,77,sequence,0,1,sizeof(payload),
        route,(uint16_t)strlen(route),payload,sizeof(payload));
    return length>0&&nb_udp_wire_decode(wire,(size_t)length,view)==0?length:-1;
}

int main(void){
    nb_udp_fec_tx_t* tx=nb_udp_fec_tx_create(8,2000);nb_udp_fec_rx_t* rx=nb_udp_fec_rx_create();
    CHECK(tx&&rx);uint8_t sources[8][1200],repairs[2][1600],recovered[1400];size_t lengths[8];
    int repair_len=0,repair_count=0;
    for(int i=0;i<8;i++){
        nb_udp_wire_view_t view;int length=make_source(sources[i],sizeof(sources[i]),100u+(uint32_t)i,
            (uint8_t)(i+1),&view);CHECK(length>0);lengths[i]=(size_t)length;
        if(i!=3&&i!=5)CHECK(nb_udp_fec_rx_note(rx,&view,1000u+(uint64_t)i)==0);
        repair_len=nb_udp_fec_tx_feed(tx,&view,1000u+(uint64_t)i,repairs[0],sizeof(repairs[0]));
        CHECK(repair_len==(i==7?repair_len:0));
        if(repair_len>0){repair_count=1;int extra=nb_udp_fec_tx_next_repair(tx,repairs[1],sizeof(repairs[1]));CHECK(extra>0);repair_count=2;}
    }
    CHECK(repair_len>0&&repair_count==2);uint32_t sid=0;uint8_t direction=0;
    CHECK(nb_udp_fec_peek(repairs[0],(size_t)repair_len,&sid,&direction)==0&&sid==77&&direction==NB_UDP_TYPE_C2S);
    int recovered_count=0;
    for(int repair_idx=0;repair_idx<repair_count;repair_idx++)for(int attempt=0;attempt<3;attempt++){
        int recovered_len=nb_udp_fec_rx_recover(rx,repairs[repair_idx],(size_t)repair_len,3000u+(uint64_t)attempt,
            recovered,sizeof(recovered));
        if(recovered_len==0)break;
        CHECK(recovered_len>0);nb_udp_wire_view_t recovered_view;
        CHECK(nb_udp_wire_decode(recovered,(size_t)recovered_len,&recovered_view)==0);
        CHECK((recovered_view.sequence==103u&&memcmp(recovered,sources[3],lengths[3])==0)||
            (recovered_view.sequence==105u&&memcmp(recovered,sources[5],lengths[5])==0));
        CHECK(nb_udp_fec_rx_seen(rx,&recovered_view)==1);recovered_count++;
    }
    CHECK(recovered_count==2);
    nb_udp_fec_rx_destroy(rx);rx=nb_udp_fec_rx_create();CHECK(rx);
    for(int i=0;i<8;i++)if(i!=2&&i!=5){nb_udp_wire_view_t view;CHECK(nb_udp_wire_decode(sources[i],lengths[i],&view)==0);CHECK(nb_udp_fec_rx_note(rx,&view,4000+i)==0);}
    CHECK(nb_udp_fec_rx_recover(rx,repairs[0],(size_t)repair_len,5000,recovered,sizeof(recovered))==0);
    nb_udp_fec_tx_destroy(tx);tx=nb_udp_fec_tx_create(8,2000);CHECK(tx);
    for(int i=0;i<3;i++){nb_udp_wire_view_t view;CHECK(nb_udp_wire_decode(sources[i],lengths[i],&view)==0);CHECK(nb_udp_fec_tx_feed(tx,&view,10000+i,repairs[0],sizeof(repairs[0]))==0);}
    CHECK(nb_udp_fec_tx_next_deadline(tx)==12000);
    CHECK(nb_udp_fec_tx_flush_due(tx,11999,repairs[0],sizeof(repairs[0]))==0);
    CHECK(nb_udp_fec_tx_flush_due(tx,12000,repairs[0],sizeof(repairs[0]))>0);
    CHECK(nb_udp_fec_tx_next_repair(tx,repairs[1],sizeof(repairs[1]))>0);
    nb_udp_fec_adaptive_t adaptive={0};
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.0,50000,1,1000000)==0);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.30,50000,1,2000000)==0);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.31,1000,1,3000000)==1);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.0,50000,1,32999999)==1);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.0,50000,1,33000000)==0);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.0,50000,1,34000000)==0);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.31,0,1,35000000)==1);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.0,0,0,65000000)==1);
    CHECK(nb_udp_fec_adaptive_update(&adaptive,0.0,50000,1,65000000)==0);
    nb_udp_fec_tx_destroy(tx);nb_udp_fec_rx_destroy(rx);puts("nb_udp_fec_test: ok");return 0;
}
