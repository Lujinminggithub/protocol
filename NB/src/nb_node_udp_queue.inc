static size_t udp_queue_limit_for_direction_values(nb_flow_class_t flow_class,int to_down,
    int egress_active,uint64_t egress_rate_bps,uint64_t egress_reorder_us,
    int ingress_active,uint64_t ingress_rate_bps,uint64_t ingress_reorder_us){
    int active=to_down?egress_active:ingress_active;
    uint64_t rate=to_down?egress_rate_bps:ingress_rate_bps;
    uint64_t reorder=to_down?egress_reorder_us:ingress_reorder_us;
    return nb_live_udp_queue_limit(flow_class,active?rate:0,active?reorder:0);
}

#ifdef NB_NODE_QUEUE_TEST
size_t nb_node_udp_queue_limit_for_test(nb_flow_class_t flow_class,int to_down,
    int egress_active,uint64_t egress_rate_bps,uint64_t egress_reorder_us,
    int ingress_active,uint64_t ingress_rate_bps,uint64_t ingress_reorder_us){
    return udp_queue_limit_for_direction_values(flow_class,to_down,egress_active,egress_rate_bps,
        egress_reorder_us,ingress_active,ingress_rate_bps,ingress_reorder_us);
}
#else
static size_t udp_queue_limit_for_direction(const proxy_stream_t* p,int to_down){
    return udp_queue_limit_for_direction_values(p->flow_class,to_down,
        nb_current_instance->transport_egress.active,
        nb_current_instance->transport_egress.link.target_rate_bps,
        nb_current_instance->transport_egress.link.reorder_delay_us,
        nb_current_instance->transport_ingress.active,
        nb_current_instance->transport_ingress.link.target_rate_bps,
        nb_current_instance->transport_ingress.link.reorder_delay_us);
}

static int dgramq_append(uint8_t** buf,size_t* len,size_t* cap,const uint8_t* d,size_t n,
    uint32_t ps_id,size_t queue_limit){
    size_t need=10+n;
    if(*len+need>queue_limit){
        log4c_warn("id=%u datagram queue over limit %zu+%zu > %zu",ps_id,*len,need,queue_limit);
        return -2;
    }
    if(nb_current_instance->queue_bytes>nb_current_instance->queue_limit_bytes||
        need>nb_current_instance->queue_limit_bytes-nb_current_instance->queue_bytes){
        log4c_warn("id=%u datagram instance queue budget exceeded",ps_id);return -3;
    }
    if(*len+need>*cap){
        size_t ncap=*cap?(*cap*2):4096;while(ncap<*len+need)ncap*=2;
        uint8_t* nb=realloc(*buf,ncap);
        if(nb==NULL){log4c_error("id=%u fec datagram queue realloc %zu fail",ps_id,ncap);return -1;}
        *buf=nb;*cap=ncap;
    }
    nb_current_instance->queue_bytes+=(uint64_t)need;nb_put_u16(*buf+*len,(uint16_t)n);
    nb_put_u64(*buf+*len+2,picoquic_current_time());memcpy(*buf+*len+10,d,n);*len+=need;return 0;
}

static int dgramq_peek(uint8_t* buf,size_t len,const uint8_t** d,size_t* n,uint64_t* queued_at){
    if(len<10)return 0;
    *n=nb_get_u16(buf);
    if(len<10+*n)return 0;
    if(queued_at)*queued_at=nb_get_u64(buf+2);
    *d=buf+10;
    return 1;
}

static void dgramq_consume(uint8_t* buf,size_t* len){
    const uint8_t* d=NULL;size_t n=0;if(!dgramq_peek(buf,*len,&d,&n,NULL))return;
    size_t consumed=10+n;memmove(buf,buf+consumed,*len-consumed);*len-=consumed;instance_queue_release(consumed);
}

static uint64_t dgramq_drop_expired(uint8_t* buf,size_t* len,uint64_t deadline_us,uint64_t now){
    uint64_t dropped=0,queued_at=0;const uint8_t* data=NULL;size_t data_len=0;
    while(dgramq_peek(buf,*len,&data,&data_len,&queued_at)&&queued_at>0&&now>queued_at&&
        now-queued_at>deadline_us){dgramq_consume(buf,len);dropped++;}
    return dropped;
}

static void udp_queue_note_pressure(proxy_stream_t* p,const uint8_t* buf,size_t length,
    size_t queue_limit,uint64_t packets){
    if(packets==0)return;
    if(UINT64_MAX-p->udp_queue_pressure_dropped<packets)p->udp_queue_pressure_dropped=UINT64_MAX;
    else p->udp_queue_pressure_dropped+=packets;
    nb_metrics_note_udp_queue_pressure_drop(&g_metrics_state,packets);
    uint64_t now=picoquic_current_time();
    if(p->udp_queue_pressure_last_warn_at!=0&&now-p->udp_queue_pressure_last_warn_at<1000000ULL)return;
    p->udp_queue_pressure_last_warn_at=now;
    uint64_t oldest_age_us=nb_udp_queue_oldest_age_us(buf,length,now);
    log4c_info("媒体队列压力 queue-pressure role=%s id=%u target=%s:%d dropped=%llu queue_bytes=%zu queue_limit=%zu oldest_age_ms=%.1f",
        role_name(G.role),p->id,p->udp_target_host,p->udp_target_port,(unsigned long long)packets,
        length,queue_limit,oldest_age_us/1000.0);
}

static int udp_queue_make_media_room(proxy_stream_t* p,uint8_t* buf,size_t* len,size_t need,
    size_t queue_limit){
    if(p->flow_class!=NB_FLOW_CLASS_MEDIA)return 0;
    uint64_t dropped=0;
    while(*len>0&&(*len+need>queue_limit||
        nb_current_instance->queue_bytes>nb_current_instance->queue_limit_bytes||
        need>nb_current_instance->queue_limit_bytes-nb_current_instance->queue_bytes)){
        size_t removed=0;uint64_t packets=0;
        if(nb_udp_queue_drop_oldest_packet(buf,len,&removed,&packets)!=0)return -1;
        instance_queue_release(removed);dropped+=packets;
    }
    udp_queue_note_pressure(p,buf,*len,queue_limit,dropped);
    if(*len+need>queue_limit||nb_current_instance->queue_bytes>nb_current_instance->queue_limit_bytes||
        need>nb_current_instance->queue_limit_bytes-nb_current_instance->queue_bytes){
        udp_queue_note_pressure(p,buf,*len,queue_limit,1);return 1;
    }
    return 0;
}

static int udp_queue_wire_raw(proxy_stream_t* p,int to_down,const uint8_t* data,size_t length,
    size_t queue_limit){
    uint8_t** buf=to_down?&p->udp_down_tx:&p->udp_up_tx;
    size_t* len=to_down?&p->udp_down_tx_len:&p->udp_up_tx_len;
    size_t* cap=to_down?&p->udp_down_tx_cap:&p->udp_up_tx_cap;
    picoquic_cnx_t* cnx=to_down?p->down_cnx:p->up_cnx;
    if(cnx==NULL||dgramq_append(buf,len,cap,data,length,p->id,queue_limit)!=0)return -1;
    if(to_down&&*len>p->media_down_q_peak)p->media_down_q_peak=*len;
    if(!to_down&&*len>p->media_up_q_peak)p->media_up_q_peak=*len;
    if(picoquic_mark_datagram_ready(cnx,1)!=0)return -1;
    return 0;
}

static int udp_queue_wire(proxy_stream_t* p,int to_down,const uint8_t* data,size_t length){
    uint8_t* buf=to_down?p->udp_down_tx:p->udp_up_tx;
    size_t* len=to_down?&p->udp_down_tx_len:&p->udp_up_tx_len;
    size_t queue_limit=udp_queue_limit_for_direction(p,to_down);
    int room=udp_queue_make_media_room(p,buf,len,NB_UDP_QUEUE_RECORD_HEADER+length,queue_limit);
    return room==0?udp_queue_wire_raw(p,to_down,data,length,queue_limit):room;
}

static int udp_queue_optional_wire(proxy_stream_t* p,int to_down,const uint8_t* data,size_t length){
    size_t len=to_down?p->udp_down_tx_len:p->udp_up_tx_len;
    size_t need=NB_UDP_QUEUE_RECORD_HEADER+length;
    size_t queue_limit=udp_queue_limit_for_direction(p,to_down);
    if(len+need>queue_limit||nb_current_instance->queue_bytes>nb_current_instance->queue_limit_bytes||
        need>nb_current_instance->queue_limit_bytes-nb_current_instance->queue_bytes)return 1;
    return udp_queue_wire_raw(p,to_down,data,length,queue_limit);
}

static int udp_queue_packet(proxy_stream_t* p,int to_down,uint8_t type,const char* route,
    const uint8_t* payload,size_t payload_length){
    char routed[NB_UDP_ROUTE_MAX];const char* wire_route=route;
    if(to_down&&p->tenant_name[0]){if(nb_tenant_wire_render(routed,sizeof(routed),p->tenant_name,route)<0)return -1;wire_route=routed;}
    uint16_t fragments=nb_udp_fragment_count(payload_length);size_t route_length=wire_route?strlen(wire_route):0;
    if(fragments==0||route_length==0||route_length>=NB_UDP_ROUTE_MAX)return -1;
    size_t need=payload_length+(size_t)fragments*(NB_UDP_QUEUE_RECORD_HEADER+24+route_length);
    uint8_t* queue=to_down?p->udp_down_tx:p->udp_up_tx;
    size_t* queue_len=to_down?&p->udp_down_tx_len:&p->udp_up_tx_len;
    size_t queue_limit=udp_queue_limit_for_direction(p,to_down);
    int room=udp_queue_make_media_room(p,queue,queue_len,need,queue_limit);if(room!=0)return room;
    uint32_t sequence=++p->udp_tx_sequence;
    for(uint16_t i=0;i<fragments;i++){
        size_t offset=(size_t)i*NB_UDP_FRAGMENT_PAYLOAD;
        size_t part=payload_length-offset;if(part>NB_UDP_FRAGMENT_PAYLOAD)part=NB_UDP_FRAGMENT_PAYLOAD;
        uint8_t wire[NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32];
        int n=nb_udp_wire_encode(wire,sizeof(wire),type,p->udp_session_id,sequence,i,fragments,
            (uint16_t)payload_length,wire_route,(uint16_t)route_length,payload+offset,(uint16_t)part);
        if(n<0||udp_queue_wire_raw(p,to_down,wire,(size_t)n,queue_limit)!=0)return -1;
    }
    return 0;
}

static int udp_fec_record_source(proxy_stream_t* p,const nb_udp_wire_view_t* view,const char* route){
    if(p==NULL||p->udp_fec_tx==NULL||view==NULL||route==NULL)return 0;
    char routed[NB_UDP_ROUTE_MAX];const char* wire_route=route;
    if(p->tenant_name[0]){if(nb_tenant_wire_render(routed,sizeof(routed),p->tenant_name,route)<0)return -1;wire_route=routed;}
    nb_udp_wire_view_t forwarded=*view;forwarded.route=(const uint8_t*)wire_route;
    forwarded.route_length=(uint16_t)strlen(wire_route);uint8_t repair[1280];
    int length=nb_udp_fec_tx_feed(p->udp_fec_tx,&forwarded,picoquic_current_time(),repair,sizeof(repair));
    if(length<0)return -1;
    p->udp_fec_source_packets++;
    if(length>0){
        int queued=udp_queue_optional_wire(p,1,repair,(size_t)length);
        if(queued<0)return -1;
        if(queued>0)return 0;
        p->udp_fec_repairs_sent++;
    }
    return 0;
}

static int udp_fec_flush_due(proxy_stream_t* p,uint64_t now){
    if(p==NULL||p->udp_fec_tx==NULL)return 0;
    uint8_t repair[1280];
    int length=nb_udp_fec_tx_flush_due(p->udp_fec_tx,now,repair,sizeof(repair));
    if(length<0)return -1;
    if(length>0){
        int queued=udp_queue_optional_wire(p,1,repair,(size_t)length);
        if(queued<0)return -1;
        if(queued>0)return 0;
        p->udp_fec_repairs_sent++;
    }
    return 0;
}

static int udp_forward_fragment(proxy_stream_t* p,int to_down,const nb_udp_wire_view_t* view,const char* route){
    char routed[NB_UDP_ROUTE_MAX];const char* wire_route=route;
    if(to_down&&p->tenant_name[0]){if(nb_tenant_wire_render(routed,sizeof(routed),p->tenant_name,route)<0)return -1;wire_route=routed;}
    size_t route_length=strlen(wire_route);uint8_t wire[NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32];
    int n=nb_udp_wire_encode(wire,sizeof(wire),view->type,view->session_id,view->sequence,
        view->fragment_index,view->fragment_count,view->total_length,wire_route,(uint16_t)route_length,
        view->payload,view->payload_length);
    return n<0?-1:udp_queue_wire(p,to_down,wire,(size_t)n);
}
#endif
