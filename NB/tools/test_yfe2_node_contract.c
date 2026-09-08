#include "nb_yfe2_node.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1;} } while(0)
typedef struct {int step,record_step,submit_step,queue_step,record,submitted,queued,priority,fail;} test_t;
static int record(void* p,const nb_udp_wire_view_t* source,uint64_t now,
    nb_yfe2_encode_job_t* sealed){test_t* t=p;t->record++;t->record_step=++t->step;
    if(!source||!now||t->fail)return -1;
    memset(sealed,0,sizeof(*sealed));sealed->connection_generation=7;
    sealed->session_id=source->session_id;sealed->actual_count=1;return 1;}
static int submit(void* p,const nb_yfe2_encode_job_t* job){test_t* t=p;t->submitted++;
    t->submit_step=++t->step;return job&&job->connection_generation==7&&!t->fail?0:-1;}
static int queue(void* p,const uint8_t* wire,size_t length,int priority){test_t* t=p;t->queued++;
    t->queue_step=++t->step;t->priority=priority;return wire&&length&&!t->fail?0:-1;}
int main(void){test_t test={0};nb_yfe2_node_ops_t ops={record,submit,queue,&test};uint8_t payload=1;
    nb_udp_wire_view_t source={NB_UDP_TYPE_C2S,1,1,0,1,1,(const uint8_t*)"T:a:1",5,&payload,1};
    CHECK(nb_yfe2_node_on_original_queued(&ops,&source,1)==0&&test.record==1);
    CHECK(test.submitted==1&&test.record_step<test.submit_step);
    nb_yfe2_encode_result_t result={0};result.connection_generation=7;result.parity_count=1;
    result.parity[0].length=1;result.parity[0].wire[0]=1;
    CHECK(nb_yfe2_node_drain_result(&ops,&result,6)==0&&test.queued==0);
    CHECK(nb_yfe2_node_drain_result(&ops,&result,7)==0&&test.queued==1&&test.priority==NB_PRIO_FEC);
    test.fail=1;CHECK(nb_yfe2_node_on_original_queued(&ops,&source,2)==0);
    CHECK(test.record==2&&test.submitted==1);
    CHECK(nb_yfe2_node_drain_result(&ops,&result,7)==0);
    CHECK(nb_yfe2_control_target((const uint8_t*)NB_YFE2_CONTROL_ROUTE,
        (uint16_t)strlen(NB_YFE2_CONTROL_ROUTE))==1);
    CHECK(nb_yfe2_control_target((const uint8_t*)"T:other:9",9)==0);
    CHECK(nb_yfe2_required_datagram(1000)==1216);
    CHECK(nb_yfe2_sender_eligible(2,"nb-yfe2-optional",NB_YFE2_ROLE_MIDDLE,1,
        NB_UDP_TYPE_C2S,1,0,1216,1000)==1);
    CHECK(nb_yfe2_sender_eligible(2,"nb-yfe2-optional",NB_YFE2_ROLE_MIDDLE,1,
        NB_UDP_TYPE_C2S,1,0,1215,1000)==0);
    CHECK(nb_yfe2_sender_eligible(1,"nb-yfe2-optional",NB_YFE2_ROLE_MIDDLE,1,
        NB_UDP_TYPE_C2S,1,0,1216,1000)==0);
    CHECK(nb_yfe2_sender_eligible(2,"nb-yfe2-optional",NB_YFE2_ROLE_ENTRY,1,
        NB_UDP_TYPE_C2S,1,0,1216,1000)==0);
    CHECK(nb_yfe2_sender_eligible(2,"nb-yfe2-optional",NB_YFE2_ROLE_MIDDLE,1,
        NB_UDP_TYPE_S2C,1,0,1216,1000)==0);
    CHECK(nb_yfe2_sender_eligible(2,"nb-yfe2-optional",NB_YFE2_ROLE_MIDDLE,1,
        NB_UDP_TYPE_C2S,0,0,1216,1000)==0);
    CHECK(nb_yfe2_sender_eligible(2,"nb-yfe2-optional",NB_YFE2_ROLE_MIDDLE,1,
        NB_UDP_TYPE_C2S,1,1,1216,1000)==0);
    CHECK(nb_yfe2_prepare_action(1216,900,1400)==NB_YFE2_PREPARE_WAIT);
    CHECK(nb_yfe2_prepare_action(1216,1216,1400)==NB_YFE2_PREPARE_SEND);
    CHECK(nb_yfe2_prepare_action(1216,900,1200)==NB_YFE2_PREPARE_FALLBACK);
    CHECK(nb_yfe2_start_action(1216,1536,1136)==NB_YFE2_START_WAIT);
    CHECK(nb_yfe2_start_action(1216,1536,1340)==NB_YFE2_START_NEGOTIATE);
    CHECK(nb_yfe2_start_action(1216,1200,1136)==NB_YFE2_START_FALLBACK);
    CHECK(nb_yfe2_counter_advanced(10,10)==0);
    CHECK(nb_yfe2_counter_advanced(10,11)==1);
    CHECK(nb_yfe2_counter_advanced(10,2)==0);
    CHECK(nb_yfe2_rate_cap_limited(118750,200000,5000000)==1);
    CHECK(nb_yfe2_rate_cap_limited(118749,200000,5000000)==0);
    CHECK(nb_yfe2_rate_cap_limited(1,0,5000000)==0);
    const char* probe_sink="T:nb-probe-sink.internal:9";
    const char* probe_source="U:t,T:nb-probe-source.internal:9";
    const char* probe_echo="T:nb-probe-echo.internal:9";
    const char* media_route="T:rtc-access-sg.tiktokv.com:443";
    CHECK(nb_yfe2_probe_route((const uint8_t*)probe_sink,(uint16_t)strlen(probe_sink))==1);
    CHECK(nb_yfe2_probe_route((const uint8_t*)probe_source,(uint16_t)strlen(probe_source))==1);
    CHECK(nb_yfe2_probe_route((const uint8_t*)probe_echo,(uint16_t)strlen(probe_echo))==1);
    CHECK(nb_yfe2_probe_route((const uint8_t*)media_route,(uint16_t)strlen(media_route))==0);
    uint8_t control_wire[512];nb_yfe2_control_t control={0},control_decoded={0};
    control.type=NB_YFE2_CTL_PROPOSE;control.nonce=0x12345678ULL;
    nb_yfe2_default_profile(&control.profile);
    int control_length=nb_yfe2_control_datagram_encode(control_wire,sizeof(control_wire),
        41,&control);
    CHECK(control_length>0);
    nb_udp_wire_view_t control_view;
    CHECK(nb_yfe2_control_datagram_decode(control_wire,(size_t)control_length,
        &control_view,&control_decoded)==0);
    CHECK(control_view.type==NB_UDP_TYPE_C2S&&control_view.session_id==41);
    CHECK(control_decoded.type==NB_YFE2_CTL_PROPOSE&&control_decoded.nonce==control.nonce);
    control.type=NB_YFE2_CTL_ACCEPT;
    control_length=nb_yfe2_control_datagram_encode(control_wire,sizeof(control_wire),41,&control);
    CHECK(control_length>0&&nb_yfe2_control_datagram_decode(control_wire,(size_t)control_length,
        &control_view,&control_decoded)==0&&control_view.type==NB_UDP_TYPE_S2C);
    puts("nb_yfe2_node_contract_test: ok");return 0;}
