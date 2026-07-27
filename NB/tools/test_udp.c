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

    nb_udp_reassembly_init(&rs);
    {
        int n=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,8,12,0,count,
            sizeof(source),route,strlen(route),source,NB_UDP_FRAGMENT_PAYLOAD);
        nb_udp_wire_view_t view;CHECK(n>0&&nb_udp_wire_decode(wire,n,&view)==0);
        CHECK(nb_udp_reassembly_feed(&rs,&view,100,&complete)==0);
        CHECK(nb_udp_reassembly_feed(&rs,&view,101,&complete)==0);
        uint8_t conflicting[1400];memcpy(conflicting,wire,(size_t)n);conflicting[21]--;
        CHECK(nb_udp_wire_decode(conflicting,(size_t)n,&view)==0);
        CHECK(nb_udp_reassembly_feed(&rs,&view,102,&complete)==-1);

        n=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,8,13,0,count,
            sizeof(source),route,strlen(route),source,NB_UDP_FRAGMENT_PAYLOAD);
        CHECK(n>0&&nb_udp_wire_decode(wire,n,&view)==0);
        CHECK(nb_udp_reassembly_feed(&rs,&view,200,&complete)==0);
        size_t off=NB_UDP_FRAGMENT_PAYLOAD;
        n=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,8,13,1,count,
            sizeof(source),route,strlen(route),source+off,NB_UDP_FRAGMENT_PAYLOAD);
        CHECK(n>0&&nb_udp_wire_decode(wire,n,&view)==0);
        CHECK(nb_udp_reassembly_feed(&rs,&view,200+NB_UDP_REASSEMBLY_TIMEOUT_US,&complete)==0);
        off+=NB_UDP_FRAGMENT_PAYLOAD;
        n=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,8,13,2,count,
            sizeof(source),route,strlen(route),source+off,(uint16_t)(sizeof(source)-off));
        CHECK(n>0&&nb_udp_wire_decode(wire,n,&view)==0);
        CHECK(nb_udp_reassembly_feed(&rs,&view,201+NB_UDP_REASSEMBLY_TIMEOUT_US,&complete)==0);
    }
    nb_udp_reassembly_dispose(&rs);

    { uint8_t payload=0;int n=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_CLOSE,7,0,0,1,1,
          route,strlen(route),&payload,1);nb_udp_wire_view_t view;
      CHECK(n>0&&nb_udp_wire_decode(wire,n,&view)==0&&view.type==NB_UDP_TYPE_CLOSE); }
    { uint8_t payload[1000]={0};nb_udp_wire_view_t view;
      int n=nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,7,14,0,2,1001,
          route,strlen(route),payload,sizeof(payload));
      CHECK(n>0);wire[19]=3;CHECK(nb_udp_wire_decode(wire,(size_t)n,&view)!=0);
      CHECK(nb_udp_wire_encode(wire,sizeof(wire),NB_UDP_TYPE_C2S,7,14,0,3,1001,
          route,strlen(route),payload,sizeof(payload))<0); }

    uint8_t socks[3000];int sn=nb_socks_udp_encode(socks,sizeof(socks),"rtc.example",443,source,100);
    CHECK(sn>0);char host[256];int port=0;const uint8_t* payload=NULL;size_t payload_len=0;
    CHECK(nb_socks_udp_parse(socks,sn,host,sizeof(host),&port,&payload,&payload_len)==0);
    CHECK(strcmp(host,"rtc.example")==0&&port==443&&payload_len==100&&memcmp(payload,source,100)==0);
    sn=nb_socks_udp_encode(socks,sizeof(socks),"192.0.2.10",5353,source,100);
    CHECK(sn>0&&nb_socks_udp_parse(socks,sn,host,sizeof(host),&port,&payload,&payload_len)==0);
    CHECK(strcmp(host,"192.0.2.10")==0&&port==5353&&payload_len==100);
    sn=nb_socks_udp_encode(socks,sizeof(socks),"2001:db8::10",5353,source,100);
    CHECK(sn>0&&nb_socks_udp_parse(socks,sn,host,sizeof(host),&port,&payload,&payload_len)==0);
    CHECK(strcmp(host,"2001:db8::10")==0&&port==5353&&payload_len==100);
    socks[2]=1;CHECK(nb_socks_udp_parse(socks,sn,host,sizeof(host),&port,&payload,&payload_len)!=0);
    CHECK(!nb_udp_control_grace_expired(100,100,100+NB_UDP_CONTROL_GRACE_US-1,
        NB_UDP_CONTROL_GRACE_US));
    CHECK(nb_udp_control_grace_expired(100,100,100+NB_UDP_CONTROL_GRACE_US,
        NB_UDP_CONTROL_GRACE_US));
    CHECK(!nb_udp_control_grace_expired(100,1000,1000+NB_UDP_CONTROL_GRACE_US-1,
        NB_UDP_CONTROL_GRACE_US));
    CHECK(nb_udp_control_grace_expired(100,1000,1000+NB_UDP_CONTROL_GRACE_US,
        NB_UDP_CONTROL_GRACE_US));
    CHECK(!nb_udp_control_grace_expired(100,1000,1000+NB_UDP_CONTROL_GRACE_US/2,
        NB_UDP_CONTROL_GRACE_US));
    CHECK(!nb_udp_control_grace_expired(100,1000+NB_UDP_CONTROL_GRACE_US/2,
        1000+NB_UDP_CONTROL_GRACE_US,NB_UDP_CONTROL_GRACE_US));
    puts("nb_udp_test: ok");return 0;
}
