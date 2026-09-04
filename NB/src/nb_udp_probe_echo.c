#include "nb_udp_probe_echo.h"

#include <string.h>

static int ascii_case_equal(const char* left,const char* right){
    if(left==NULL||right==NULL)return 0;
    while(*left&&*right){
        unsigned char a=(unsigned char)*left++,b=(unsigned char)*right++;
        if(a>='A'&&a<='Z')a=(unsigned char)(a-'A'+'a');
        if(b>='A'&&b<='Z')b=(unsigned char)(b-'A'+'a');
        if(a!=b)return 0;
    }
    return *left==0&&*right==0;
}

int nb_probe_internal_target(const char* host,int port){
    return port==NB_PROBE_PORT&&(ascii_case_equal(host,NB_PROBE_SINK_HOST)||
        ascii_case_equal(host,NB_PROBE_ECHO_HOST)||ascii_case_equal(host,NB_PROBE_SOURCE_HOST));
}

int nb_udp_probe_echo_target(const char* host,int port){
    return nb_probe_internal_target(host,port)&&ascii_case_equal(host,NB_PROBE_ECHO_HOST);
}

int nb_udp_probe_echo_plan(const char* host,int port,const nb_udp_reassembled_t* complete,
    nb_udp_probe_echo_plan_t* plan){
    if(host==NULL||complete==NULL||plan==NULL)return -1;
    memset(plan,0,sizeof(*plan));
    if(!nb_udp_probe_echo_target(host,port))return 0;
    if(complete->route==NULL||complete->route_length==0||complete->payload==NULL||
        complete->payload_length==0)return -1;
    plan->internal_echo_ready=1;
    plan->type=NB_UDP_TYPE_S2C;plan->sequence=complete->sequence;
    plan->route=complete->route;plan->route_length=complete->route_length;
    plan->payload=complete->payload;plan->payload_length=complete->payload_length;
    plan->c2s_packets_delta=1;plan->s2c_packets_delta=1;
    plan->c2s_bytes_delta=complete->payload_length;plan->s2c_bytes_delta=complete->payload_length;
    return 1;
}

int nb_udp_probe_echo_exit_feed(const char* host,int port,nb_udp_reassembly_t* reassembly,
    const nb_udp_wire_view_t* view,uint64_t now_us,nb_udp_probe_echo_enqueue_fn enqueue,void* opaque){
    if(host==NULL||reassembly==NULL||view==NULL||enqueue==NULL)return -1;
    if(!nb_udp_probe_echo_target(host,port))return 0;
    if(view->type!=NB_UDP_TYPE_C2S)return -1;
    nb_udp_reassembled_t complete;
    int reassembled=nb_udp_reassembly_feed(reassembly,view,now_us,&complete);
    if(reassembled<0)return NB_UDP_PROBE_ECHO_REASSEMBLY_ERROR;
    if(reassembled==0)return 0;
    nb_udp_probe_echo_plan_t plan;
    if(nb_udp_probe_echo_plan(host,port,&complete,&plan)!=1)return -1;
    int queued=enqueue(opaque,&plan);
    return queued<0?-1:(queued>0?0:1);
}
