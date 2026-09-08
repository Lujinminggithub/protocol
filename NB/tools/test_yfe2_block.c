#include "nb_yfe2_block.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1;} } while(0)

static void source(uint32_t sequence,uint8_t fill,uint8_t* payload,nb_udp_wire_view_t* view){
    memset(payload,fill,900);*view=(nb_udp_wire_view_t){NB_UDP_TYPE_C2S,77,sequence,0,1,900,
        (const uint8_t*)"T:live.example:50001",20,payload,900};
}

int main(void){
    nb_yfe2_tx_t tx;nb_yfe2_tx_init(&tx,42,77);nb_yfe2_encode_job_t job;
    uint8_t payload[64][900];nb_udp_wire_view_t views[64];
    for(uint32_t i=0;i<8;i++){source(i+1,(uint8_t)i,payload[i],&views[i]);
        CHECK(nb_yfe2_tx_add(&tx,&views[i],1000+i,1,&job)==0);}
    CHECK(nb_yfe2_tx_slot_count(&tx,0)==2&&nb_yfe2_tx_slot_count(&tx,1)==2);
    CHECK(nb_yfe2_tx_slot_count(&tx,2)==2&&nb_yfe2_tx_slot_count(&tx,3)==2);
    CHECK(nb_yfe2_tx_flush_due(&tx,10999,&job)==0);
    CHECK(nb_yfe2_tx_flush_due(&tx,11000,&job)==1);
    CHECK(job.actual_count==2&&job.parity_shards==1&&job.connection_generation==42);
    CHECK(job.desc[0].sequence==1&&job.desc[1].sequence==5);

    nb_yfe2_tx_init(&tx,43,77);int sealed=0;
    for(uint32_t i=0;i<64;i++){source(i+1,(uint8_t)i,payload[i],&views[i]);
        int rc=nb_yfe2_tx_add(&tx,&views[i],20000+i,1,&job);CHECK(rc>=0);sealed+=rc;}
    CHECK(sealed==4&&job.actual_count==16&&job.parity_shards==1);
    nb_yfe2_tx_set_parity(&tx,3);
    source(65,65,payload[0],&views[0]);CHECK(nb_yfe2_tx_add(&tx,&views[0],30000,3,&job)==0);
    CHECK(nb_yfe2_tx_flush_due(&tx,40000,&job)==1&&job.parity_shards==3);
    nb_yfe2_encode_result_t result;CHECK(nb_yfe2_encode_job_run(&job,&result)==0);
    CHECK(result.parity_count==3&&result.connection_generation==43);
    for(int i=0;i<3;i++){nb_yfe2_parity_view_t parity;
        CHECK(nb_yfe2_wire_decode(result.parity[i].wire,result.parity[i].length,&parity)==0);
        CHECK(parity.parity_shards==3&&parity.shard_index==16+i&&parity.actual_data_count==1);}
    puts("nb_yfe2_block_test: ok");return 0;
}
