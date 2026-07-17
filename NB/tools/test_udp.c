#include "nb_udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

int main(void){
    uint8_t source[2501];for(size_t i=0;i<sizeof(source);i++)source[i]=(uint8_t)(i*31u);
    const char* route="T:rtc-access.example:443";uint16_t count=nb_udp_fragment_count(sizeof(source));
    CHECK(count==3);nb_udp_reassembly_t rs;nb_udp_reassembly_init(&rs);nb_udp_reassembled_t complete;
    uint8_t wire[1400];int order[3]={2,0,1};
    for(int oi=0;oi<3;oi++){
        int idx=order[oi];size_t off=(size_t)idx*NB_UDP_FRAGMENT_PAYLOAD;
        size_t len=sizeof(source)-off;if(len>NB_UDP_FRAGMENT_PAYLOAD)len=NB_UDP_FRAGMENT_PAYLOAD;
        int n=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,7,11,(uint16_t)idx,count,
            sizeof(source),route,strlen(route),source+off,(uint16_t)len);
        CHECK(n>0);nb_udp_wire_view_t view;CHECK(nb_udp_wire_decode(wire,n,&view)==0);
        int rc=nb_udp_reassembly_feed(&rs,&view,100+oi,&complete);CHECK(rc==(oi==2?1:0));
    }
    CHECK(complete.payload_length==sizeof(source));CHECK(memcmp(complete.payload,source,sizeof(source))==0);
    CHECK(strcmp(complete.route,route)==0);nb_udp_reassembly_dispose(&rs);

    uint8_t socks[3000];int sn=nb_socks_udp_encode(socks,sizeof(socks),"rtc.example",443,source,100);
    CHECK(sn>0);char host[256];int port=0;const uint8_t* payload=NULL;size_t payload_len=0;
    CHECK(nb_socks_udp_parse(socks,sn,host,sizeof(host),&port,&payload,&payload_len)==0);
    CHECK(strcmp(host,"rtc.example")==0&&port==443&&payload_len==100&&memcmp(payload,source,100)==0);
    socks[2]=1;CHECK(nb_socks_udp_parse(socks,sn,host,sizeof(host),&port,&payload,&payload_len)!=0);
    puts("nb_udp_test: ok");return 0;
}
