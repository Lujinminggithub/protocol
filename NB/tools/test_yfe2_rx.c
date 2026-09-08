#include "nb_yfe2_rx.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1;} } while(0)

static int build(uint8_t r,nb_udp_wire_view_t views[16],uint8_t payload[16][900],
    nb_yfe2_encode_result_t* result){
    nb_yfe2_tx_t tx;nb_yfe2_tx_init(&tx,9,77);nb_yfe2_encode_job_t job;int sealed=0;
    for(uint32_t i=0;i<64;i++){
        size_t index=i/4;memset(payload[index],(int)(index+1),900);
        views[index]=(nb_udp_wire_view_t){NB_UDP_TYPE_C2S,77,index+100,0,1,900,
            (const uint8_t*)"T:live.example:50001",20,payload[index],900};
        int rc=nb_yfe2_tx_add(&tx,&views[index],1000+i,r,&job);
        if((i%4)==0&&rc==1){sealed=1;break;}
    }
    return sealed?nb_yfe2_encode_job_run(&job,result):-1;
}

static int recover(uint8_t r,const int* missing,size_t missing_count){
    nb_udp_wire_view_t views[16];uint8_t payload[16][900];nb_yfe2_encode_result_t encoded;
    CHECK(build(r,views,payload,&encoded)==0);nb_yfe2_rx_t* rx=nb_yfe2_rx_create();CHECK(rx);
    for(size_t i=0;i<16;i++){int skip=0;for(size_t j=0;j<missing_count;j++)skip|=(int)i==missing[j];
        if(!skip)CHECK(nb_yfe2_rx_note_source(rx,9,&views[i],10000)==0);}
    nb_yfe2_recovered_t out[3];size_t count=0;
    for(uint8_t i=0;i<encoded.parity_count;i++){
        nb_yfe2_parity_view_t parity;CHECK(nb_yfe2_wire_decode(encoded.parity[i].wire,encoded.parity[i].length,&parity)==0);
        int rc=nb_yfe2_rx_add_parity(rx,9,&parity,"T:live.example:50001",20,11000+i,out,3,&count);
        CHECK(rc>=0);
    }
    CHECK(count==missing_count);
    for(size_t i=0;i<count;i++){nb_udp_wire_view_t view;CHECK(nb_udp_wire_decode(out[i].wire,out[i].length,&view)==0);
        int found=-1;for(size_t j=0;j<missing_count;j++)if(view.sequence==views[missing[j]].sequence)found=missing[j];
        CHECK(found>=0&&view.payload_length==900&&!memcmp(view.payload,payload[found],900));
        CHECK(nb_yfe2_rx_note_source(rx,9,&views[found],12000)==1);}
    nb_yfe2_rx_metrics_t metrics;nb_yfe2_rx_snapshot(rx,&metrics);
    CHECK(metrics.decode_ns>0&&metrics.memory_high>0);
    nb_yfe2_rx_destroy(rx);return 0;
}

int main(void){
    const int one[]={5};CHECK(recover(1,one,1)==0);
    const int three[]={2,7,11};CHECK(recover(3,three,3)==0);
    nb_udp_wire_view_t views[16];uint8_t payload[16][900];nb_yfe2_encode_result_t encoded;
    CHECK(build(1,views,payload,&encoded)==0);
    nb_yfe2_rx_t* rx=nb_yfe2_rx_create();CHECK(rx);nb_yfe2_rx_metrics_t metrics={0};
    for(size_t i=0;i<14;i++)CHECK(nb_yfe2_rx_note_source(rx,9,&views[i],10000)==0);
    nb_yfe2_parity_view_t parity;
    CHECK(nb_yfe2_wire_decode(encoded.parity[0].wire,encoded.parity[0].length,&parity)==0);
    nb_yfe2_recovered_t out[3];size_t count=0;
    CHECK(nb_yfe2_rx_add_parity(rx,9,&parity,"T:live.example:50001",20,11000,
        out,3,&count)==0&&count==0);
    CHECK(nb_yfe2_rx_next_deadline(rx)==161000);
    CHECK(nb_yfe2_rx_expire(rx,160999,&metrics)==0);
    CHECK(nb_yfe2_rx_expire(rx,161000,&metrics)==1&&metrics.unrecoverable==1);
    CHECK(nb_yfe2_rx_next_deadline(rx)==0);
    nb_yfe2_rx_destroy(rx);
    nb_yfe2_rx_t* receivers[128]={0};size_t receiver_count=0;
    while(receiver_count<128&&(receivers[receiver_count]=nb_yfe2_rx_create())!=NULL)
        receiver_count++;
    CHECK(receiver_count>0&&receiver_count<128);
    for(size_t i=0;i<receiver_count;i++)nb_yfe2_rx_destroy(receivers[i]);
    rx=nb_yfe2_rx_create();CHECK(rx);nb_yfe2_rx_destroy(rx);
    nb_yfe2_rx_t* block_receivers[9];
    for(size_t i=0;i<9;i++){block_receivers[i]=nb_yfe2_rx_create();CHECK(block_receivers[i]);}
    CHECK(build(1,views,payload,&encoded)==0);
    uint64_t global_evictions=0;
    for(size_t receiver=0;receiver<9;receiver++)for(uint32_t block=1;block<=29;block++){
        uint8_t wire[NB_YFE2_MAX_PARITY_WIRE];size_t length=encoded.parity[0].length;
        memcpy(wire,encoded.parity[0].wire,length);uint32_t block_id=(uint32_t)(receiver*29+block);
        wire[12]=(uint8_t)(block_id>>24);wire[13]=(uint8_t)(block_id>>16);
        wire[14]=(uint8_t)(block_id>>8);wire[15]=(uint8_t)block_id;
        CHECK(nb_yfe2_wire_decode(wire,length,&parity)==0);count=0;
        CHECK(nb_yfe2_rx_add_parity(block_receivers[receiver],9,&parity,
            "T:live.example:50001",20,20000+block_id,out,3,&count)>=0);
    }
    for(size_t i=0;i<9;i++){nb_yfe2_rx_snapshot(block_receivers[i],&metrics);
        global_evictions+=metrics.evicted;nb_yfe2_rx_destroy(block_receivers[i]);}
    CHECK(global_evictions>0);
    puts("nb_yfe2_rx_test: ok");return 0;
}
