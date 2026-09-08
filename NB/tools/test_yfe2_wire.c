#include "nb_yfe2_wire.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){ \
    fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1; } } while(0)

static int reject_mutation(const uint8_t* source,size_t length,size_t offset,uint8_t value){
    uint8_t wire[1400];nb_yfe2_parity_view_t view;
    memcpy(wire,source,length);wire[offset]=value;
    return nb_yfe2_wire_decode(wire,length,&view)!=NB_YFE2_WIRE_OK;
}

int main(void){
    nb_yfe2_desc_t desc[2]={{100,0,2,1800,1000},{100,1,2,1800,800}};
    uint8_t parity[1000],wire[1400];memset(parity,0x5a,sizeof(parity));
    nb_yfe2_parity_t value={0};value.flags=0;value.profile_id=28909;
    value.session_id=77;value.block_id=91;value.direction=NB_YFE2_DIRECTION_C2S;
    value.shard_index=16;value.data_shards=16;value.parity_shards=1;
    value.actual_data_count=2;value.shard_size=1000;value.desc=desc;value.body=parity;
    int length=nb_yfe2_wire_encode(wire,sizeof(wire),&value);CHECK(length==1048);
    CHECK(!memcmp(wire,"NBUF",4)&&wire[4]==3&&wire[5]==0);
    CHECK(wire[6]==0x70&&wire[7]==0xed&&wire[17]==16&&wire[18]==16);
    CHECK(wire[19]==1&&wire[20]==2&&wire[21]==0);
    nb_yfe2_parity_view_t decoded;CHECK(nb_yfe2_wire_decode(wire,(size_t)length,&decoded)==NB_YFE2_WIRE_OK);
    CHECK(decoded.session_id==77&&decoded.block_id==91&&decoded.actual_data_count==2);
    CHECK(decoded.desc[0].sequence==100&&decoded.desc[1].payload_length==800);
    CHECK(decoded.body_length==1000&&decoded.body[999]==0x5a);
    CHECK(reject_mutation(wire,(size_t)length,0,'X'));
    CHECK(reject_mutation(wire,(size_t)length,4,2));
    CHECK(reject_mutation(wire,(size_t)length,5,0x80));
    CHECK(reject_mutation(wire,(size_t)length,7,0xee));
    CHECK(reject_mutation(wire,(size_t)length,16,NB_YFE2_DIRECTION_S2C));
    CHECK(reject_mutation(wire,(size_t)length,17,15));
    CHECK(reject_mutation(wire,(size_t)length,18,15));
    CHECK(reject_mutation(wire,(size_t)length,19,2));
    CHECK(reject_mutation(wire,(size_t)length,20,0));
    CHECK(reject_mutation(wire,(size_t)length,21,1));
    CHECK(nb_yfe2_wire_decode(wire,(size_t)length-1,&decoded)==NB_YFE2_WIRE_LENGTH);
    value.flags=NB_YFE2_FLAG_BURST;value.parity_shards=3;value.shard_index=18;
    length=nb_yfe2_wire_encode(wire,sizeof(wire),&value);CHECK(length==1048);
    CHECK(nb_yfe2_wire_decode(wire,(size_t)length,&decoded)==NB_YFE2_WIRE_OK);
    puts("nb_yfe2_wire_test: ok");return 0;
}
