#include "nb_v2_metadata.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

static nb_v2_flow_metadata_t sample(void){
    nb_v2_flow_metadata_t metadata={0};metadata.flags=NB_V2_META_FLAG_ALLOW_FEC|NB_V2_META_FLAG_ALLOW_MULTIPATH;
    metadata.traffic_class=NB_V2_META_CLASS_REALTIME;metadata.priority=6;
    metadata.path_preference=NB_V2_META_PATH_LOW_JITTER;metadata.session_id=0x0102030405060708ULL;
    metadata.flow_id=0x1112131415161718ULL;metadata.deadline_ms=200;
    metadata.policy_id=42;metadata.route="H:middle.example:4443,H:exit.example:4443";
    metadata.target="live.example:443";metadata.business_tag="tiktok.live";return metadata;
}

int main(void){
    uint8_t wire[1024];nb_v2_flow_metadata_t metadata=sample();
    CHECK(nb_v2_flow_metadata_validate(&metadata));
    int encoded=nb_v2_flow_metadata_encode(wire,sizeof(wire),&metadata);CHECK(encoded>0);
    static const uint8_t expected_header[NB_V2_META_HEADER_SIZE]={
        0x4e,0x42,0x4d,0x32,0x01,0x01,0x00,0x30,0x00,0x00,0x00,0x03,0x02,0x06,0x02,0x00,
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x00,0x00,0x00,0xc8,0x00,0x00,0x00,0x2a,0x00,0x29,0x00,0x10,0x00,0x0b,0x00,0x00,
    };
    CHECK(memcmp(wire,expected_header,sizeof(expected_header))==0);
    nb_v2_flow_metadata_view_t view;CHECK(nb_v2_flow_metadata_decode(wire,(size_t)encoded,&view)==0);
    CHECK(view.flags==metadata.flags&&view.traffic_class==metadata.traffic_class&&
        view.priority==metadata.priority&&view.path_preference==metadata.path_preference);
    CHECK(view.session_id==metadata.session_id&&view.flow_id==metadata.flow_id&&
        view.deadline_ms==metadata.deadline_ms&&view.policy_id==metadata.policy_id);
    CHECK(view.route_length==strlen(metadata.route)&&memcmp(view.route,metadata.route,view.route_length)==0);
    CHECK(view.target_length==strlen(metadata.target)&&memcmp(view.target,metadata.target,view.target_length)==0);
    CHECK(view.business_tag_length==strlen(metadata.business_tag)&&
        memcmp(view.business_tag,metadata.business_tag,view.business_tag_length)==0);

    CHECK(nb_v2_flow_metadata_encode(wire,(size_t)encoded-1,&metadata)<0);
    for(size_t truncated=0;truncated<(size_t)encoded;truncated++)
        CHECK(nb_v2_flow_metadata_decode(wire,truncated,&view)<0);
    CHECK(nb_v2_flow_metadata_decode(wire,(size_t)encoded+1,&view)<0);
    wire[4]=2;CHECK(nb_v2_flow_metadata_decode(wire,(size_t)encoded,&view)<0);wire[4]=NB_V2_META_VERSION;
    wire[15]=1;CHECK(nb_v2_flow_metadata_decode(wire,(size_t)encoded,&view)<0);wire[15]=0;
    wire[8]|=0x80;CHECK(nb_v2_flow_metadata_decode(wire,(size_t)encoded,&view)<0);wire[8]&=0x7f;
    wire[NB_V2_META_HEADER_SIZE]='\n';CHECK(nb_v2_flow_metadata_decode(wire,(size_t)encoded,&view)<0);

    metadata=sample();metadata.session_id=0;CHECK(!nb_v2_flow_metadata_validate(&metadata));
    metadata=sample();metadata.deadline_ms=0;CHECK(!nb_v2_flow_metadata_validate(&metadata));
    metadata=sample();metadata.traffic_class=NB_V2_META_CLASS_RELIABLE;
    CHECK(!nb_v2_flow_metadata_validate(&metadata));
    metadata.flags=0;metadata.deadline_ms=0;CHECK(nb_v2_flow_metadata_validate(&metadata));
    metadata=sample();metadata.path_preference=NB_V2_META_PATH_REDUNDANT;
    metadata.flags=NB_V2_META_FLAG_ALLOW_FEC;CHECK(!nb_v2_flow_metadata_validate(&metadata));
    metadata=sample();metadata.business_tag="invalid tag";CHECK(!nb_v2_flow_metadata_validate(&metadata));
    metadata=sample();metadata.route="bad\nroute";CHECK(!nb_v2_flow_metadata_validate(&metadata));
    puts("nb_v2_metadata_test: ok");return 0;
}
