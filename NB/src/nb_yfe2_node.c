#include "nb_yfe2_node.h"

#include <string.h>

int nb_yfe2_node_on_original_queued(nb_yfe2_node_ops_t* ops,
    const nb_udp_wire_view_t* source,uint64_t now_us){
    nb_yfe2_encode_job_t sealed;
    int record_result;
    if(ops==NULL||source==NULL)return -1;
    if(ops->record_source==NULL)return 0;
    memset(&sealed,0,sizeof(sealed));
    record_result=ops->record_source(ops->context,source,now_us,&sealed);
    if(record_result<=0)return 0;
    if(ops->submit_job!=NULL)(void)ops->submit_job(ops->context,&sealed);
    return 0;
}
int nb_yfe2_node_drain_result(nb_yfe2_node_ops_t* ops,
    const nb_yfe2_encode_result_t* result,uint64_t current_generation){
    if(ops==NULL||result==NULL)return -1;
    if(result->connection_generation!=current_generation)return 0;
    for(uint8_t i=0;i<result->parity_count;i++){
        if(ops->queue_parity&&ops->queue_parity(ops->context,result->parity[i].wire,
            result->parity[i].length,NB_PRIO_FEC)!=0)return 0;
    }
    return 0;
}
int nb_yfe2_control_target(const uint8_t* route,uint16_t route_length){
    static const char expected[]=NB_YFE2_CONTROL_ROUTE;
    return route&&route_length==sizeof(expected)-1&&!memcmp(route,expected,sizeof(expected)-1);
}

static int route_has_segment(const uint8_t* route,uint16_t route_length,const char* segment){
    size_t segment_length=strlen(segment);
    if(route==NULL||segment==NULL||segment_length==0)return 0;
    for(size_t offset=0;offset<route_length;){
        size_t end=offset;
        while(end<route_length&&route[end]!=',')end++;
        if(end-offset==segment_length&&!memcmp(route+offset,segment,segment_length))return 1;
        offset=end+1;
    }
    return 0;
}

int nb_yfe2_probe_route(const uint8_t* route,uint16_t route_length){
    return route_has_segment(route,route_length,"T:nb-probe-sink.internal:9")||
        route_has_segment(route,route_length,"T:nb-probe-source.internal:9")||
        route_has_segment(route,route_length,"T:nb-probe-echo.internal:9");
}

size_t nb_yfe2_required_datagram(size_t shard_size){
    if(shard_size>SIZE_MAX-NB_YFE2_WIRE_HEADER-
        NB_YFE2_DATA_SHARDS*NB_YFE2_WIRE_DESC)return SIZE_MAX;
    return NB_YFE2_WIRE_HEADER+NB_YFE2_DATA_SHARDS*NB_YFE2_WIRE_DESC+shard_size;
}

int nb_yfe2_sender_eligible(uint32_t schema_version,const char* fec_mode,int role,
    int outbound,uint8_t direction,int media,int probe,size_t max_datagram_payload,
    size_t shard_size){
    return schema_version==2&&fec_mode!=NULL&&!strcmp(fec_mode,"nb-yfe2-optional")&&
        role==NB_YFE2_ROLE_MIDDLE&&outbound&&direction==NB_UDP_TYPE_C2S&&media&&!probe&&
        nb_yfe2_required_datagram(shard_size)<=max_datagram_payload;
}

int nb_yfe2_prepare_action(size_t wire_length,size_t allowance,
    size_t max_datagram_payload){
    if(wire_length==0||max_datagram_payload<wire_length)return NB_YFE2_PREPARE_FALLBACK;
    return allowance<wire_length?NB_YFE2_PREPARE_WAIT:NB_YFE2_PREPARE_SEND;
}

int nb_yfe2_start_action(size_t required_payload,size_t advertised_payload,
    size_t current_path_payload){
    if(required_payload==0)return NB_YFE2_START_FALLBACK;
    if(advertised_payload==0)return NB_YFE2_START_WAIT;
    if(advertised_payload<required_payload)
        return NB_YFE2_START_FALLBACK;
    return current_path_payload<required_payload?NB_YFE2_START_WAIT:
        NB_YFE2_START_NEGOTIATE;
}

int nb_yfe2_counter_advanced(uint64_t previous,uint64_t current){
    return current>previous;
}

int nb_yfe2_control_datagram_encode(uint8_t* out,size_t cap,uint32_t session_id,
    const nb_yfe2_control_t* control){
    uint8_t payload[NB_YFE2_CONTROL_SIZE];
    int payload_length;
    uint8_t type;
    if(out==NULL||control==NULL||session_id==0)return -1;
    payload_length=nb_yfe2_control_encode(payload,sizeof(payload),control);
    if(payload_length<0)return -1;
    type=control->type==NB_YFE2_CTL_PROPOSE?NB_UDP_TYPE_C2S:NB_UDP_TYPE_S2C;
    return nb_udp_wire_encode(out,cap,type,session_id,1,0,1,(uint16_t)payload_length,
        NB_YFE2_CONTROL_ROUTE,(uint16_t)(sizeof(NB_YFE2_CONTROL_ROUTE)-1),payload,
        (uint16_t)payload_length);
}

int nb_yfe2_control_datagram_decode(const uint8_t* wire,size_t length,
    nb_udp_wire_view_t* view,nb_yfe2_control_t* control){
    if(nb_udp_wire_decode(wire,length,view)!=0||
        !nb_yfe2_control_target(view->route,view->route_length)||
        view->fragment_index!=0||view->fragment_count!=1||
        view->payload_length!=NB_YFE2_CONTROL_SIZE||
        nb_yfe2_control_decode(view->payload,view->payload_length,control)!=0)return -1;
    if((control->type==NB_YFE2_CTL_PROPOSE&&view->type!=NB_UDP_TYPE_C2S)||
        (control->type!=NB_YFE2_CTL_PROPOSE&&view->type!=NB_UDP_TYPE_S2C))return -1;
    return 0;
}
