static proxy_stream_t* udp_exit_open(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view,const char* route){
    char tenant[NB_TENANT_NAME_MAX];const char* routed=route;
    int tenant_wire=nb_tenant_wire_parse(route,tenant,sizeof(tenant),&routed);if(tenant_wire<0)return NULL;
    char host[256];int port=0;if(parse_target_from_route(routed,host,sizeof(host),&port)!=0)return NULL;
    int internal_echo=nb_udp_probe_echo_target(host,port);
    if(!internal_echo&&!whitelist_allowed(host,port))return NULL;
    proxy_stream_t* p=ps_alloc();if(p==NULL)return NULL;
    p->udp_mode=1;p->udp_session_id=view->session_id;p->up_cnx=cnx;
    if(g_tenants_enabled){
        p->tenant_index=tenant_wire>0?nb_tenant_find(&g_tenants,tenant):(g_tenants.count==1?0:-1);
        if(p->tenant_index<0){ps_set_close_reason(p,"tenant-wire-unknown");ps_free(p);return NULL;}
        snprintf(p->tenant_name,sizeof(p->tenant_name),"%s",g_tenants.items[p->tenant_index].name);
        if(tenant_wire==0)log4c_warn("exit UDP accepted legacy tenant-less flow using sole tenant=%s",p->tenant_name);
    }
    if(udp_reassembly_ensure(p)!=0){ps_set_close_reason(p,"udp-exit-state-allocation");ps_free(p);return NULL;}
    ps_index_refresh(p);
    snprintf(p->route,sizeof(p->route),"%s",routed);snprintf(p->udp_target_host,sizeof(p->udp_target_host),"%s",host);
    p->udp_target_port=port;p->target_port=port;nb_flow_policy_t pol;flow_policy_for_host(host,port,&pol);apply_flow_policy(p,&pol);
    if(internal_echo){
        p->probe_mode=2;p->target_connect_state=2;p->udp_target_ready_at=picoquic_current_time();
        log4c_info("exit UDP 内部回显就绪 id=%u usid=%u target=%s:%d",p->id,p->udp_session_id,host,port);
    }else{
        if(dns_submit(p->id,host,port)!=0){ps_set_close_reason(p,"udp-dns-queue-full");ps_free(p);return NULL;}
        p->dns_pending=1;
    }
    log4c_info("exit udp flow open id=%u usid=%u target=%s:%d",p->id,p->udp_session_id,host,port);return p;
}

static int udp_exit_enqueue_probe_echo(void* opaque,const nb_udp_probe_echo_plan_t* echo){
    proxy_stream_t* p=opaque;if(p==NULL||echo==NULL)return -1;
    int queue_rc=udp_queue_packet_with_sequence(p,0,echo->type,echo->route,echo->payload,
        echo->payload_length,echo->sequence,1);
    if(queue_rc<0)return -1;
    if(queue_rc>0)return 1;
    p->udp_packets_c2s+=echo->c2s_packets_delta;p->bytes_c2s+=echo->c2s_bytes_delta;
    p->udp_packets_s2c+=echo->s2c_packets_delta;p->bytes_s2c+=echo->s2c_bytes_delta;
    ps_touch(p);return 0;
}

static int udp_deliver_reassembled(proxy_stream_t* p,const nb_udp_reassembled_t* complete){
    if(G.role==ROLE_EXIT){
        if(p->udp_fd>=0){
            ssize_t n=send(p->udp_fd,complete->payload,complete->payload_length,MSG_NOSIGNAL);
            if(n!=(ssize_t)complete->payload_length)return -1;
            if(p->udp_first_target_send_at==0){
                p->udp_first_target_send_at=picoquic_current_time();
                log4c_info("exit udp trace id=%u usid=%u stage=target-first-send age=%.1fms ready_to_send=%.1fms bytes=%zu target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_target_send_at,p->created_at),
                    trace_delta_ms(p->udp_first_target_send_at,p->udp_target_ready_at),complete->payload_length,
                    p->udp_target_host,p->udp_target_port);
            }
        }else if(udp_queue_pending_append(p,complete->payload,complete->payload_length)!=0)return -1;
        p->udp_packets_c2s++;p->bytes_c2s+=complete->payload_length;ps_touch(p);return 0;
    }
    if(G.role==ROLE_ENTRY){
        proxy_stream_t* parent=ps_find_by_id(p->udp_parent_id);
        if(parent==NULL||!parent->udp_association||!parent->udp_client_peer_set)return -1;
        char host[256];int port=0;if(parse_target_from_route(complete->route,host,sizeof(host),&port)!=0)return -1;
        uint8_t socks[NB_UDP_MAX_PAYLOAD+300];int n=nb_socks_udp_encode(socks,sizeof(socks),host,port,
            complete->payload,complete->payload_length);if(n<0)return -1;
        if(sendto(parent->udp_fd,socks,(size_t)n,MSG_NOSIGNAL,(struct sockaddr*)&parent->udp_client_peer,
            parent->udp_client_peer_len)!=n)return -1;
        if(p->udp_first_local_s2c_at==0){
            p->udp_first_local_s2c_at=picoquic_current_time();
            log4c_info("entry udp trace id=%u usid=%u stage=phone-first-send age=%.1fms rx_to_send=%.1fms bytes=%zu target=%s:%d",
                p->id,p->udp_session_id,trace_delta_ms(p->udp_first_local_s2c_at,p->created_at),
                trace_delta_ms(p->udp_first_local_s2c_at,p->udp_first_s2c_rx_at),complete->payload_length,
                p->udp_target_host,p->udp_target_port);
        }
        p->udp_packets_s2c++;p->bytes_s2c+=complete->payload_length;ps_touch(p);ps_touch(parent);return 0;
    }
    return -1;
}

static void yfe2_stream_metrics_snapshot(const proxy_stream_t* p,
    nb_yfe2_metrics_t* metrics){
    nb_yfe2_metrics_init(metrics);if(p==NULL)return;
    metrics->business_bytes=p->yfe2_business_bytes;metrics->wire_bytes=p->yfe2_wire_bytes;
    metrics->original=p->yfe2_original;metrics->baseline_parity=p->yfe2_baseline_parity;
    metrics->burst_parity=p->yfe2_burst_parity;
    metrics->parity_received=p->yfe2_parity_received;
    metrics->parity_queue_drop=p->yfe2_parity_queue_drop;
    metrics->pmtu_fallback=p->yfe2_pmtu_fallback;
    metrics->probe_parity_suppressed=p->yfe2_probe_parity_suppressed;
    if(p->yfe2_rx){
        nb_yfe2_rx_metrics_t receive;nb_yfe2_rx_snapshot(p->yfe2_rx,&receive);
        metrics->recovered=receive.recovered;metrics->unrecoverable=receive.unrecoverable;
        metrics->duplicate=receive.duplicate;metrics->corrupt=receive.corrupt;
        metrics->decoder_queue_drop=receive.evicted;metrics->decode_ns=receive.decode_ns;
        metrics->memory_high_bytes=receive.memory_high;
    }
}

static size_t yfe2_advertised_datagram_payload(picoquic_cnx_t* cnx){
    return cnx?(size_t)cnx->remote_parameters.max_datagram_frame_size:0;
}

static size_t yfe2_max_datagram_payload(picoquic_cnx_t* cnx){
    if(cnx==NULL)return 0;
    size_t maximum=yfe2_advertised_datagram_payload(cnx);
    picoquic_path_t* path=cnx->nb_paths>0?cnx->path[0]:NULL;
    if(path&&path->send_mtu>64){
        size_t path_payload=path->send_mtu-64;
        if(maximum==0||path_payload<maximum)maximum=path_payload;
    }
    return maximum;
}

static uint32_t yfe2_control_session_new(void){
    uint32_t session_id=0;
    for(int attempt=0;attempt<8&&session_id==0;attempt++)
        picoquic_crypto_random(G.quic,&session_id,sizeof(session_id));
    return session_id;
}

static int yfe2_queue_control(picoquic_cnx_t* cnx,nb_sched_cnx_t* runtime,
    const nb_yfe2_control_t* control){
    int length;
    if(cnx==NULL||runtime==NULL||control==NULL)return -1;
    if(runtime->yfe2_control_session==0)runtime->yfe2_control_session=yfe2_control_session_new();
    if(runtime->yfe2_control_session==0)return -1;
    length=nb_yfe2_control_datagram_encode(runtime->yfe2_control_wire,
        sizeof(runtime->yfe2_control_wire),runtime->yfe2_control_session,control);
    if(length<0)return -1;
    runtime->yfe2_control_length=(size_t)length;
    (void)picoquic_mark_datagram_ready(cnx,1);
    return 0;
}

static void yfe2_maybe_start(picoquic_cnx_t* cnx){
    nb_sched_cnx_t* runtime;
    nb_yfe2_control_t proposal={0};
    uint64_t nonce=0,now;
    const nb_transport_profile_t* profile;
    if(G.role!=ROLE_MIDDLE||cnx==NULL||!nb_current_instance->transport_profiles.has_active)return;
    profile=&nb_current_instance->transport_profiles.active;
    size_t max_payload=yfe2_max_datagram_payload(cnx);
    size_t advertised=yfe2_advertised_datagram_payload(cnx);
    size_t required=nb_yfe2_required_datagram(NB_UDP_FRAGMENT_PAYLOAD);
    int start_action=nb_yfe2_start_action(required,advertised,max_payload);
    if(profile->schema_version==2&&
        !strcmp(nb_current_instance->transport_egress.link.udp_fec_mode,"nb-yfe2-optional")&&
        start_action==NB_YFE2_START_FALLBACK){
        runtime=runtime_for_cnx(cnx,1);
        if(runtime&&runtime->yfe2_negotiation.state==NB_YFE2_NEG_DISABLED)
            runtime->yfe2_negotiation.state=NB_YFE2_NEG_FALLBACK_PMTU;
        return;
    }
    if(start_action!=NB_YFE2_START_NEGOTIATE)return;
    if(!nb_yfe2_sender_eligible(profile->schema_version,
        nb_current_instance->transport_egress.link.udp_fec_mode,G.role,1,
        NB_UDP_TYPE_C2S,1,0,max_payload,NB_UDP_FRAGMENT_PAYLOAD))return;
    runtime=runtime_for_cnx(cnx,1);
    if(runtime==NULL||runtime->yfe2_negotiation.state!=NB_YFE2_NEG_DISABLED)return;
    for(int attempt=0;attempt<8&&nonce==0;attempt++)picoquic_crypto_random(G.quic,&nonce,sizeof(nonce));
    now=picoquic_current_time();
    if(nonce==0||nb_yfe2_negotiation_start(&runtime->yfe2_negotiation,nonce,now)!=0)return;
    proposal.type=NB_YFE2_CTL_PROPOSE;proposal.nonce=nonce;
    nb_yfe2_default_profile(&proposal.profile);
    if(yfe2_queue_control(cnx,runtime,&proposal)!=0){
        (void)nb_yfe2_negotiation_reject(&runtime->yfe2_negotiation,nonce,
            NB_YFE2_REASON_RESOURCE);
        log4c_warn("middle NB YFE2 协商入队失败 generation=%llu",
            (unsigned long long)runtime->yfe2_generation);
    }else log4c_info("middle NB YFE2 发起协商 generation=%llu profile=%u",
        (unsigned long long)runtime->yfe2_generation,NB_YFE2_PROFILE_ID);
}

static int yfe2_handle_control(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view){
    nb_yfe2_control_t control;
    nb_sched_cnx_t* runtime;
    if(view==NULL||!nb_yfe2_control_target(view->route,view->route_length))return 0;
    if(nb_yfe2_control_decode(view->payload,view->payload_length,&control)!=0){
        log4c_warn("%s NB YFE2 协商包无效",role_name(G.role));return 1;
    }
    runtime=runtime_for_cnx(cnx,1);if(runtime==NULL)return 1;
    if(G.role==ROLE_EXIT&&view->type==NB_UDP_TYPE_C2S&&control.type==NB_YFE2_CTL_PROPOSE){
        nb_yfe2_control_t response=control;response.type=NB_YFE2_CTL_ACCEPT;
        if(nb_yfe2_negotiation_start(&runtime->yfe2_negotiation,control.nonce,
            picoquic_current_time())==0&&nb_yfe2_negotiation_accept(&runtime->yfe2_negotiation,
            control.nonce,control.profile_id)==0){
            runtime->yfe2_control_session=view->session_id;
            if(yfe2_queue_control(cnx,runtime,&response)==0)
                log4c_info("exit NB YFE2 接受协商 generation=%llu profile=%u",
                    (unsigned long long)runtime->yfe2_generation,control.profile_id);
        }
        return 1;
    }
    if(G.role==ROLE_MIDDLE&&view->type==NB_UDP_TYPE_S2C&&
        runtime->yfe2_negotiation.state==NB_YFE2_NEG_PROPOSED){
        int result=control.type==NB_YFE2_CTL_ACCEPT?
            nb_yfe2_negotiation_accept(&runtime->yfe2_negotiation,control.nonce,
                control.profile_id):
            (control.type==NB_YFE2_CTL_REJECT?
                nb_yfe2_negotiation_reject(&runtime->yfe2_negotiation,control.nonce,
                    control.reason):-1);
        if(result==0)log4c_info("middle NB YFE2 协商结果 generation=%llu state=%s",
            (unsigned long long)runtime->yfe2_generation,
            nb_yfe2_negotiation_state_name(runtime->yfe2_negotiation.state));
        return 1;
    }
    return 1;
}

typedef struct {proxy_stream_t* stream;nb_sched_cnx_t* runtime;} yfe2_original_context_t;

static int yfe2_record_source(void* opaque,const nb_udp_wire_view_t* source,uint64_t now_us,
    nb_yfe2_encode_job_t* sealed){
    yfe2_original_context_t* context=opaque;proxy_stream_t* p=context->stream;
    if(p->yfe2_tx==NULL){
        p->yfe2_tx=calloc(1,sizeof(*p->yfe2_tx));
        if(p->yfe2_tx==NULL)return -1;
    }
    if(p->yfe2_generation!=context->runtime->yfe2_generation){
        nb_yfe2_tx_init(p->yfe2_tx,context->runtime->yfe2_generation,p->udp_session_id);
        p->yfe2_generation=context->runtime->yfe2_generation;
    }
    uint8_t parity=context->runtime->yfe2_adaptive.mode==NB_YFE2_MODE_BURST?
        NB_YFE2_BURST_PARITY:NB_YFE2_BASE_PARITY;
    int result=nb_yfe2_tx_add(p->yfe2_tx,source,now_us,parity,sealed);
    if(result>=0){p->udp_fec_source_packets++;p->yfe2_original++;
        p->yfe2_business_bytes+=source->payload_length;
        context->runtime->yfe2_business_bytes+=source->payload_length;
        p->yfe2_wire_bytes+=source->payload_length;}
    return result;
}

static int yfe2_submit_job(void* opaque,const nb_yfe2_encode_job_t* job){
    yfe2_original_context_t* context=opaque;
    int result=g_yfe2_worker?nb_yfe2_worker_submit(g_yfe2_worker,job):-1;
    if(result!=0)context->stream->yfe2_encoder_queue_drop++;
    return result;
}

static void yfe2_on_original_queued(proxy_stream_t* p,const nb_udp_wire_view_t* view,
    const char* route,uint64_t now_us){
    nb_sched_cnx_t* runtime;
    yfe2_original_context_t context;
    nb_yfe2_node_ops_t ops;
    if(p==NULL||view==NULL||route==NULL||G.role!=ROLE_MIDDLE)return;
    if(nb_yfe2_probe_route((const uint8_t*)route,(uint16_t)strlen(route))){
        nb_sched_cnx_t* probe_runtime=runtime_for_cnx(p->down_cnx,0);
        if(probe_runtime){
            probe_runtime->yfe2_probe_packets++;
        }
        if(probe_runtime&&probe_runtime->yfe2_negotiation.state==NB_YFE2_NEG_ACCEPTED)
            p->yfe2_probe_parity_suppressed++;
        return;
    }
    if(p->flow_class!=NB_FLOW_CLASS_MEDIA)return;
    yfe2_maybe_start(p->down_cnx);
    runtime=runtime_for_cnx(p->down_cnx,0);
    if(runtime==NULL||runtime->yfe2_negotiation.state!=NB_YFE2_NEG_ACCEPTED)return;
    context=(yfe2_original_context_t){p,runtime};
    ops=(nb_yfe2_node_ops_t){yfe2_record_source,yfe2_submit_job,NULL,&context};
    (void)nb_yfe2_node_on_original_queued(&ops,view,now_us);
}

static void yfe2_receive_parity(picoquic_cnx_t* cnx,const uint8_t* wire,size_t length){
    nb_yfe2_parity_view_t parity;
    nb_sched_cnx_t* runtime=runtime_for_cnx(cnx,0);
    if(G.role!=ROLE_EXIT||runtime==NULL||
        runtime->yfe2_negotiation.state!=NB_YFE2_NEG_ACCEPTED)return;
    if(nb_yfe2_wire_decode(wire,length,&parity)!=NB_YFE2_WIRE_OK){
        log4c_warn("exit NB YFE2 parity 拒绝 length=%zu",length);return;
    }
    proxy_stream_t* p=ps_find_udp_up(cnx,parity.session_id);if(p==NULL||
        p->flow_class!=NB_FLOW_CLASS_MEDIA||p->probe_mode)return;
    if(p->yfe2_rx==NULL)p->yfe2_rx=nb_yfe2_rx_create();
    if(p->yfe2_rx==NULL)return;
    nb_yfe2_recovered_t recovered[NB_YFE2_BURST_PARITY];size_t count=0;
    int result=nb_yfe2_rx_add_parity(p->yfe2_rx,runtime->yfe2_generation,&parity,
        p->route,(uint16_t)strlen(p->route),picoquic_current_time(),recovered,
        NB_YFE2_BURST_PARITY,&count);
    p->udp_fec_repairs_received++;p->yfe2_parity_received++;
    if(result<0)return;
    for(size_t i=0;i<count;i++){
        nb_udp_wire_view_t view;
        if(nb_udp_wire_decode(recovered[i].wire,recovered[i].length,&view)==0&&
            udp_on_quic_datagram(cnx,&view)==0){
            p->udp_fec_recovered++;
            log4c_info("exit NB YFE2 恢复 id=%u usid=%u seq=%u frag=%u/%u",
                p->id,p->udp_session_id,view.sequence,view.fragment_index+1,
                view.fragment_count);
        }
    }
}

typedef struct {proxy_stream_t* stream;} yfe2_parity_context_t;

static int yfe2_queue_parity(void* opaque,const uint8_t* wire,size_t length,int priority){
    yfe2_parity_context_t* context=opaque;
    proxy_stream_t* p=context->stream;
    if(priority!=NB_PRIO_FEC)return -1;
    size_t need=NB_UDP_QUEUE_RECORD_HEADER+length;
    int result=1;
    if(p->yfe2_parity_tx_len+need<=NB_DGRAM_QUEUE_MAX&&
        nb_current_instance->queue_bytes<=nb_current_instance->queue_limit_bytes&&
        need<=nb_current_instance->queue_limit_bytes-nb_current_instance->queue_bytes){
        result=dgramq_append(&p->yfe2_parity_tx,&p->yfe2_parity_tx_len,
            &p->yfe2_parity_tx_cap,wire,length,p->id,NB_DGRAM_QUEUE_MAX);
        if(result==0&&picoquic_mark_datagram_ready(p->down_cnx,1)!=0){
            p->yfe2_parity_tx_len-=need;instance_queue_release(need);result=-1;
        }
    }
    if(result==0){p->udp_fec_repairs_sent++;p->yfe2_wire_bytes+=length;
        if(length>5&&(wire[5]&NB_YFE2_FLAG_BURST))p->yfe2_burst_parity++;
        else p->yfe2_baseline_parity++;
    }else p->yfe2_parity_queue_drop++;
    return result;
}

static proxy_stream_t* yfe2_find_result_stream(const nb_yfe2_encode_result_t* result){
    for(int i=0;i<MAX_CONN;i++){
        proxy_stream_t* p=&G.streams[i];
        if(!p->in_use||!p->udp_mode||p->udp_session_id!=result->session_id||
            p->down_cnx==NULL||p->yfe2_generation!=result->connection_generation)continue;
        nb_sched_cnx_t* runtime=runtime_for_cnx(p->down_cnx,0);
        if(runtime&&runtime->yfe2_generation==result->connection_generation&&
            runtime->yfe2_negotiation.state==NB_YFE2_NEG_ACCEPTED)return p;
    }
    return NULL;
}

static void yfe2_drain_worker(void){
    if(g_yfe2_worker==NULL)return;
    nb_yfe2_encode_result_t result;
    while(nb_yfe2_worker_drain(g_yfe2_worker,&result)>0){
        proxy_stream_t* p=yfe2_find_result_stream(&result);if(p==NULL)continue;
        yfe2_parity_context_t context={p};
        nb_yfe2_node_ops_t ops={NULL,NULL,yfe2_queue_parity,&context};
        (void)nb_yfe2_node_drain_result(&ops,&result,p->yfe2_generation);
    }
}

static void yfe2_flush_due(uint64_t now_us){
    if(g_yfe2_worker==NULL)return;
    for(int i=0;i<MAX_CONN;i++){
        proxy_stream_t* p=&G.streams[i];
        if(!p->in_use||p->yfe2_tx==NULL)continue;
        for(;;){
            nb_yfe2_encode_job_t job;
            int result=nb_yfe2_tx_flush_due(p->yfe2_tx,now_us,&job);
            if(result<=0)break;
            (void)nb_yfe2_worker_submit(g_yfe2_worker,&job);
        }
    }
}

static uint64_t yfe2_next_deadline(void){
    uint64_t earliest=0;
    for(int i=0;i<MAX_CONN;i++){
        proxy_stream_t* p=&G.streams[i];
        if(!p->in_use)continue;
        if(p->yfe2_tx){
            uint64_t deadline=nb_yfe2_tx_next_deadline(p->yfe2_tx);
            if(deadline&&(earliest==0||deadline<earliest))earliest=deadline;
        }
        if(p->yfe2_rx){
            uint64_t deadline=nb_yfe2_rx_next_deadline(p->yfe2_rx);
            if(deadline&&(earliest==0||deadline<earliest))earliest=deadline;
        }
    }
    for(size_t i=0;i<NB_SCHED_CNX_CAP;i++){
        nb_sched_cnx_t* runtime=&g_sched_cnx[i];
        if(runtime->cnx&&runtime->yfe2_negotiation.state==NB_YFE2_NEG_PROPOSED){
            uint64_t deadline=runtime->yfe2_negotiation.deadline_us;
            if(deadline&&(earliest==0||deadline<earliest))earliest=deadline;
        }else if(runtime->cnx&&runtime->yfe2_negotiation.state==NB_YFE2_NEG_ACCEPTED){
            uint64_t deadline=runtime->yfe2_sample_at?runtime->yfe2_sample_at+200000ULL:1;
            if(earliest==0||deadline<earliest)earliest=deadline;
        }
    }
    return earliest;
}

static uint64_t yfe2_pmtu_fallbacks(picoquic_cnx_t* cnx){
    for(int pool_id=0;pool_id<G.pool_count;pool_id++)for(int slot=0;slot<POOL_SIZE;slot++)
        if(G.pools[pool_id].cnx[slot]==cnx)return G.pools[pool_id].pmtu[slot].fallbacks;
    return 0;
}

static void yfe2_adaptive_tick(uint64_t now_us){
    if(G.role!=ROLE_MIDDLE)return;
    for(size_t i=0;i<NB_SCHED_CNX_CAP;i++){
        nb_sched_cnx_t* runtime=&g_sched_cnx[i];
        if(runtime->cnx==NULL||runtime->yfe2_negotiation.state!=NB_YFE2_NEG_ACCEPTED||
            (runtime->yfe2_sample_at&&now_us-runtime->yfe2_sample_at<200000ULL))continue;
        picoquic_path_quality_t quality;memset(&quality,0,sizeof(quality));
        picoquic_get_default_path_quality(runtime->cnx,&quality);
        quality.max_reorder_delay=nb_path_sanitize_reorder_delay(quality.max_reorder_delay);
        quality.max_reorder_gap=nb_path_sanitize_reorder_gap(quality.max_reorder_gap);
        picoquic_nb_limit_state_t limit={0};
        (void)picoquic_get_nb_limit_state(runtime->cnx,&limit);
        nb_yfe2_loss_sample_t sample={0};
        sample.generation=runtime->yfe2_generation;sample.sent=quality.sent;
        sample.declared_lost=quality.lost;sample.spurious_lost=quality.spurious_losses;
        sample.send_queue_full=limit.send_queue_full;
        sample.pmtu_fallbacks=yfe2_pmtu_fallbacks(runtime->cnx);
        if(limit.app_limited)sample.invalid_reasons|=NB_YFE2_IGNORE_APP_LIMITED;
        uint64_t elapsed=runtime->yfe2_sample_at?now_us-runtime->yfe2_sample_at:0;
        uint64_t business_delta=runtime->yfe2_business_bytes>=runtime->yfe2_business_bytes_previous?
            runtime->yfe2_business_bytes-runtime->yfe2_business_bytes_previous:0;
        if(nb_yfe2_rate_cap_limited(business_delta,elapsed,
            nb_current_instance->transport_egress.link.target_rate_bps))
            sample.invalid_reasons|=NB_YFE2_IGNORE_RATE_CAP;
        runtime->yfe2_business_bytes_previous=runtime->yfe2_business_bytes;
        for(int stream=0;stream<MAX_CONN;stream++){
            proxy_stream_t* p=&G.streams[stream];
            if(!p->in_use||p->down_cnx!=runtime->cnx)continue;
            sample.local_drops+=p->udp_queue_pressure_dropped;
            sample.scheduler_expired+=p->yfe2_scheduler_expired;
            if(p->tenant_throttled_until>now_us)
                sample.invalid_reasons|=NB_YFE2_IGNORE_RATE_CAP;
        }
        if(nb_yfe2_counter_advanced(runtime->yfe2_probe_packets_previous,
            runtime->yfe2_probe_packets))
            sample.invalid_reasons|=NB_YFE2_IGNORE_PROBE;
        runtime->yfe2_probe_packets_previous=runtime->yfe2_probe_packets;
        int previous=runtime->yfe2_adaptive.mode;
        int current=nb_yfe2_adaptive_update(&runtime->yfe2_adaptive,&sample,now_us);
        runtime->yfe2_sample_at=now_us;
        if(current!=previous)log4c_info("middle NB YFE2 自适应 generation=%llu mode=%s effective_loss=%llu",
            (unsigned long long)runtime->yfe2_generation,
            current==NB_YFE2_MODE_BURST?"burst":"baseline",
            (unsigned long long)runtime->yfe2_adaptive.effective_physical_loss);
    }
}

static void yfe2_tick(uint64_t now_us){
    yfe2_flush_due(now_us);
    yfe2_adaptive_tick(now_us);
    for(int i=0;i<MAX_CONN;i++)if(G.streams[i].in_use&&G.streams[i].yfe2_rx)
        (void)nb_yfe2_rx_expire(G.streams[i].yfe2_rx,now_us,NULL);
    for(size_t i=0;i<NB_SCHED_CNX_CAP;i++){
        nb_sched_cnx_t* runtime=&g_sched_cnx[i];
        if(runtime->cnx&&runtime->yfe2_negotiation.state==NB_YFE2_NEG_PROPOSED&&
            nb_yfe2_negotiation_timeout(&runtime->yfe2_negotiation,now_us)==
                NB_YFE2_NEG_FALLBACK_TIMEOUT)
            log4c_info("%s NB YFE2 协商超时 generation=%llu，保持普通 NBUD",
                role_name(G.role),(unsigned long long)runtime->yfe2_generation);
    }
}

static int udp_on_quic_datagram(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view){
    char route[NB_UDP_ROUTE_MAX];if(view->route_length>=sizeof(route))return -2;
    memcpy(route,view->route,view->route_length);route[view->route_length]=0;
    if(yfe2_handle_control(cnx,view))return 0;
    if(view->type==NB_UDP_TYPE_CLOSE_ACK){(void)nb_udp_lifecycle_ack(&g_udp_lifecycle,(uintptr_t)cnx,view->session_id);return 0;}
    if(view->type==NB_UDP_TYPE_CLOSE){
        proxy_stream_t* p=NULL;(void)udp_lifecycle_schedule(cnx,NB_UDP_TYPE_CLOSE_ACK,view->session_id,route);
        if(G.role==ROLE_ENTRY)p=ps_find_udp_down(cnx,view->session_id);else if(G.role==ROLE_EXIT)p=ps_find_udp_up(cnx,view->session_id);
        else {p=ps_find_udp_up(cnx,view->session_id);if(p==NULL)p=ps_find_udp_down(cnx,view->session_id);}
        if(p==NULL)return 0;
        picoquic_cnx_t* opposite=p->up_cnx==cnx?p->down_cnx:p->up_cnx;
        if(opposite!=NULL)(void)udp_lifecycle_schedule(opposite,NB_UDP_TYPE_CLOSE,p->udp_session_id,p->route);
        p->udp_close_received=1;
        p->udp_close_received_at=picoquic_current_time();
        ps_set_close_reason(p,"udp-peer-close");
        ps_touch(p);
        return 0;
    }
    if(view->type==NB_UDP_TYPE_C2S){
        if(G.role==ROLE_MIDDLE){
            proxy_stream_t* p=ps_find_udp_up(cnx,view->session_id);char rest[NB_UDP_ROUTE_MAX];
            if(p==NULL)p=udp_middle_open(cnx,view,route,rest,sizeof(rest));
            else snprintf(rest,sizeof(rest),"%s",p->route);
            udp_reopen_if_needed(p);
            if(p!=NULL&&p->udp_first_c2s_rx_at==0){p->udp_first_c2s_rx_at=picoquic_current_time();
                log4c_info("middle udp trace id=%u usid=%u stage=c2s-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            if(p==NULL)return -3;
            int forward_rc=udp_forward_fragment(p,1,view,rest);
            if(forward_rc<0)return -4;
            if(forward_rc>0)return 0;
            yfe2_on_original_queued(p,view,rest,picoquic_current_time());
            nb_sched_cnx_t* yfe2_runtime=runtime_for_cnx(p->down_cnx,0);
            int yfe2_accepted=yfe2_runtime&&
                yfe2_runtime->yfe2_negotiation.state==NB_YFE2_NEG_ACCEPTED;
            if(!yfe2_accepted&&p->flow_class==NB_FLOW_CLASS_MEDIA&&nb_current_instance->transport_egress.active&&
                nb_current_instance->transport_egress.link.udp_fec_adaptive&&
                pool_udp_fec_needed(&G.pools[p->down_pool_id],picoquic_current_time())){
                if(p->udp_fec_tx==NULL){
                    const nb_transport_link_profile_t* link=&nb_current_instance->transport_egress.link;
                    uint8_t repair_count=G.pools[p->down_pool_id].udp_fec.repair_count;
                    if(repair_count<1||repair_count>NB_UDP_FEC_MAX_R)repair_count=NB_UDP_FEC_DEFAULT_R;
                    p->udp_fec_tx=nb_udp_fec_tx_create_ex((uint8_t)link->udp_fec_k,repair_count,link->udp_fec_hold_us);
                    if(p->udp_fec_tx==NULL)return -4;
                    log4c_info("middle udp FEC flow armed id=%u usid=%u k=%u r=%u hold=%lluus target=%s:%d",
                        p->id,p->udp_session_id,link->udp_fec_k,repair_count,(unsigned long long)link->udp_fec_hold_us,
                        p->udp_target_host,p->udp_target_port);
                }
                if(udp_fec_record_source(p,view,rest)!=0)return -4;
            }
            p->udp_packets_c2s+=(view->fragment_index==0);p->bytes_c2s+=view->payload_length;ps_touch(p);return 0;
        }
        if(G.role==ROLE_EXIT){
            proxy_stream_t* p=ps_find_udp_up(cnx,view->session_id);if(p==NULL)p=udp_exit_open(cnx,view,route);
            udp_reopen_if_needed(p);
            if(p==NULL)return -5;
            if(p->udp_first_c2s_rx_at==0){p->udp_first_c2s_rx_at=picoquic_current_time();
                log4c_info("exit udp trace id=%u usid=%u stage=c2s-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            nb_udp_reassembled_t complete;
            if(p->udp_reassembly==NULL)return -6;
            if(nb_udp_probe_echo_target(p->udp_target_host,p->udp_target_port)){
                int echo_rc=nb_udp_probe_echo_exit_feed(p->udp_target_host,p->udp_target_port,
                    p->udp_reassembly,view,picoquic_current_time(),udp_exit_enqueue_probe_echo,p);
                return echo_rc<0?(echo_rc==NB_UDP_PROBE_ECHO_REASSEMBLY_ERROR?-6:-7):0;
            }
            int rc=nb_udp_reassembly_feed(p->udp_reassembly,view,picoquic_current_time(),&complete);
            return rc<0?-6:(rc==1?(udp_deliver_reassembled(p,&complete)==0?0:-7):0);
        }
    }else if(view->type==NB_UDP_TYPE_S2C){
        if(G.role==ROLE_MIDDLE){
            proxy_stream_t* p=ps_find_udp_down(cnx,view->session_id);
            /* CLOSE keeps the relay session alive briefly for lifecycle ACK
             * and in-flight cleanup, but the Entry child is already gone. */
            if(p!=NULL&&p->udp_close_received)return 0;
            if(p!=NULL&&p->udp_first_s2c_rx_at==0){p->udp_first_s2c_rx_at=picoquic_current_time();
                log4c_info("middle udp trace id=%u usid=%u stage=s2c-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_s2c_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            if(p==NULL)return -8;
            int forward_rc=udp_forward_fragment(p,0,view,p->route);
            if(forward_rc<0)return -9;
            if(forward_rc>0)return 0;
            p->udp_packets_s2c+=(view->fragment_index==0);p->bytes_s2c+=view->payload_length;ps_touch(p);return 0;
        }
        if(G.role==ROLE_ENTRY){
            proxy_stream_t* p=ps_find_udp_down(cnx,view->session_id);if(p==NULL)return -10;
            if(p->udp_first_s2c_rx_at==0){p->udp_first_s2c_rx_at=picoquic_current_time();
                log4c_info("entry udp trace id=%u usid=%u stage=s2c-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_s2c_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            if(p->udp_reassembly==NULL)return -11;
            nb_udp_reassembled_t complete;int rc=nb_udp_reassembly_feed(p->udp_reassembly,view,picoquic_current_time(),&complete);
            return rc<0?-11:(rc==1?(udp_deliver_reassembled(p,&complete)==0?0:-12):0);
        }
    }
    return -13;
}

/* middle/exit: 收上一跳 stream 数据(去程)。首解析 route 首部, 再转发。 */
static int probe_sink_feed(proxy_stream_t* p,const uint8_t* bytes,size_t len){
    size_t off=0;
    while(off<len&&p->probe_header_len<NB_PROBE_HEADER_SIZE)
        p->probe_header[p->probe_header_len++]=bytes[off++];
    if(p->probe_header_len==NB_PROBE_HEADER_SIZE&&p->probe_expected_bytes==0){
        if(nb_probe_header_parse(p->probe_header,p->probe_header_len,&p->probe_expected_bytes)!=0){
            ps_set_close_reason(p,"probe-header");return -1;
        }
    }
    if(off<len){
        size_t payload=len-off;
        if(p->probe_expected_bytes==0||p->probe_ack_sent||
            payload>p->probe_expected_bytes-p->probe_bytes){
            ps_set_close_reason(p,"probe-length");return -1;
        }
        p->probe_hash=nb_probe_hash_update(p->probe_hash,bytes+off,payload);
        p->probe_bytes+=payload;ps_note_payload(p,1,payload);
        char progress[96];int n=nb_probe_progress_format(p->probe_bytes,picoquic_current_time(),
            &p->probe_progress_at,progress,sizeof(progress));
        if(n<0||(n>0&&queue_up(p,(const uint8_t*)progress,(size_t)n,0)!=0))return -1;
    }
    if(p->probe_expected_bytes!=0&&p->probe_bytes==p->probe_expected_bytes&&!p->probe_ack_sent){
        char ack[128];int n=nb_probe_ack_format(p->probe_bytes,p->probe_hash,ack,sizeof(ack));
        if(n<0||queue_up(p,(const uint8_t*)ack,(size_t)n,1)!=0)return -1;
        p->probe_ack_sent=1;
    }
    return 0;
}

static int probe_source_request(proxy_stream_t* p,const uint8_t* bytes,size_t len){
    if(p->probe_header_len+len>NB_PROBE_HEADER_SIZE||p->probe_expected_bytes!=0)return -1;
    memcpy(p->probe_header+p->probe_header_len,bytes,len);p->probe_header_len+=len;
    if(p->probe_header_len<NB_PROBE_HEADER_SIZE)return 0;
    if(nb_probe_source_header_parse(p->probe_header,p->probe_header_len,&p->probe_expected_bytes)!=0){
        ps_set_close_reason(p,"probe-source-header");return -1;
    }
    return 0;
}

static int probe_source_pump(proxy_stream_t* p){
    if(!p||p->probe_mode!=4||p->probe_expected_bytes==0||p->probe_bytes>=p->probe_expected_bytes)return 0;
    uint64_t now=picoquic_current_time();if(p->tenant_throttled_until>now)return 0;
    p->tenant_throttled_until=0;uint8_t payload[16384];size_t desired=(size_t)(p->probe_expected_bytes-p->probe_bytes);
    if(desired>sizeof(payload))desired=sizeof(payload);
    size_t allowed=g_tenants_enabled?nb_tenant_take_class(&g_tenants,p->tenant_index,
        desired,now,NB_TENANT_DOWN,p->flow_class==NB_FLOW_CLASS_MEDIA&&!p->downlink_bulk):desired;
    if(allowed==0){p->tenant_throttled_until=now+
        nb_tenant_retry_after_us(&g_tenants,p->tenant_index,1,NB_TENANT_DOWN);return 0;}
    nb_probe_source_fill(p->probe_bytes,payload,allowed);
    if(queue_up(p,payload,allowed,p->probe_bytes+allowed==p->probe_expected_bytes)!=0){
        if(g_tenants_enabled)nb_tenant_refund(&g_tenants,p->tenant_index,allowed,NB_TENANT_DOWN);
        return -1;
    }
    p->probe_hash=nb_probe_hash_update(p->probe_hash,payload,allowed);p->probe_bytes+=allowed;
    ps_note_payload(p,0,allowed);return 0;
}

static int on_up_data(proxy_stream_t** stream, uint8_t* bytes, size_t len, int fin){
    proxy_stream_t* p=stream?*stream:NULL;if(p==NULL)return -1;
    size_t off=0;
    if(p->fec_ctrl_only){
        if(len > 0) fec_ctrl_feed_bytes(p, bytes, len, 0);
        if(fin) p->up_fin_seen=1;
        return 0;
    }
    if(p->hdr_done&&p->probe_mode){
        if(len>0){
            if(p->probe_mode==1){if(probe_sink_feed(p,bytes,len)!=0)return -1;}
            else if(p->probe_mode==4){if(probe_source_request(p,bytes,len)!=0)return -1;}
            else if(queue_up(p,bytes,len,0)!=0)return -1;
        }
        if(fin){
            p->up_fin_seen=1;p->tcp_eof=1;
            if(p->probe_mode==1){
                if(!p->probe_ack_sent){
                    char ack[128];int n=nb_probe_ack_format(p->probe_bytes,p->probe_hash,ack,sizeof(ack));
                    if(n<0||queue_up(p,(const uint8_t*)ack,(size_t)n,1)!=0)return -1;
                    p->probe_ack_sent=1;
                }
            }else if(queue_up(p,NULL,0,1)!=0)return -1;
        }
        return 0;
    }
    if(!p->hdr_done){
        while(off<len && p->hdr_len<sizeof(p->hdr)-1){
            char ch=(char)bytes[off++]; if(ch=='\n'){ p->hdr[p->hdr_len]=0; p->hdr_done=1; break; }
            p->hdr[p->hdr_len++]=ch;
        }
        if(!p->hdr_done && p->hdr_len >= sizeof(p->hdr)-1){
            char preview[512];
            p->hdr[p->hdr_len] = 0;
            fmt_route_preview(p->hdr, preview, sizeof(preview));
            log4c_error("id=%u route header overflow len=%zu preview=[%s]", p->id, p->hdr_len, preview);
            ps_teardown(p);
            return -1;
        }
        if(!p->hdr_done) return 0; /* 继续等首部 */
        if(p->hdr_len>=sizeof(p->route)){
            log4c_error("id=%u route exceeds internal limit len=%zu",p->id,p->hdr_len);
            ps_teardown(p);return -1;
        }
        memcpy(p->route,p->hdr,p->hdr_len+1);
        if(strncmp(p->hdr, NB_FEC_CTRL_PREFIX, strlen(NB_FEC_CTRL_PREFIX)) == 0){
            p->fec_ctrl_only=1;
            fec_ctrl_handle_line(p, p->hdr, 0);
            if(off < len) fec_ctrl_feed_bytes(p, bytes + off, len - off, 0);
            if(fin) p->up_fin_seen=1;
            return 0;
        }
        /* 新格式 <prio>;<route>；兼容读取旧格式 <prio>;0;<route>，但不再配对双发流。 */
        int prio=NB_PRIO_BULK; char* rt=p->hdr;
        { char* semi=strchr(rt,';');
          if(semi && semi>rt){ int ok=1; for(char* q=rt;q<semi;q++) if(*q<'0'||*q>'9'){ok=0;break;}
            if(ok){ prio=atoi(rt); rt=semi+1;
                char* semi2=strchr(rt,';');
                if(semi2&&semi2>rt){int old_numeric=1;for(char* q=rt;q<semi2;q++)if(*q<'0'||*q>'9'){old_numeric=0;break;}
                    if(old_numeric)rt=semi2+1;}
            } } }
        if(prio<=0||prio>255){log4c_error("id=%u invalid priority",p->id);ps_teardown(p);return -1;}
        p->prio=prio;
        picoquic_set_stream_priority(p->up_cnx,p->up_stream_id,(uint8_t)prio);
        { char tenant[NB_TENANT_NAME_MAX];const char* routed=rt;int tenant_wire=nb_tenant_wire_parse(rt,tenant,sizeof(tenant),&routed);
          if(tenant_wire<0){ps_teardown_reason(p,"tenant-route-header");return -1;}
          if(tenant_wire>0){snprintf(p->tenant_name,sizeof(p->tenant_name),"%s",tenant);rt=(char*)routed;}
          if(G.role==ROLE_EXIT&&g_tenants_enabled){
              p->tenant_index=tenant_wire>0?nb_tenant_find(&g_tenants,tenant):(g_tenants.count==1?0:-1);
              if(p->tenant_index<0){ps_teardown_reason(p,"tenant-route-unknown");return -1;}
              snprintf(p->tenant_name,sizeof(p->tenant_name),"%s",g_tenants.items[p->tenant_index].name);
              if(tenant_wire==0)log4c_warn("exit TCP accepted legacy tenant-less flow using sole tenant=%s",p->tenant_name);
          }
        }
        if(!strncmp(rt,"L=",2)){
            char* logical_end=strchr(rt+2,';');uint64_t flow_hi=0,flow_lo=0;
            if(logical_end==NULL||logical_end-(rt+2)!=32){ps_teardown_reason(p,"logical-route-header");return -1;}
            char flow[33];memcpy(flow,rt+2,32);flow[32]=0;
            if(nb_lstream_flow_parse(flow,&flow_hi,&flow_lo)!=0){ps_teardown_reason(p,"logical-flow-id");return -1;}
            p->logical_mode=1;p->logical_flow_hi=flow_hi;p->logical_flow_lo=flow_lo;rt=logical_end+1;
            if(G.role==ROLE_EXIT){proxy_stream_t* existing=ps_find_logical(flow_hi,flow_lo,p);
                if(existing!=NULL){p=logical_rebind_upstream(p,existing);*stream=p;
                    if(off<len&&logical_receive(p,bytes+off,len-off)!=0){ps_teardown_reason(p,"logical-resume-frame");return -1;}
                    return 0;}
                if(logical_endpoint_init(p,flow_hi,flow_lo)!=0){ps_teardown_reason(p,"logical-init");return -1;}}
        }
        /* 首部(去 prio 前缀后)第一段(到第一个 ',') = 本节点动作; 其余 = 转发给下一跳 */
        char first[300], rest[300]; rest[0]=0;
        char* comma=strchr(rt,',');
        if(comma){
            size_t fl=(size_t)(comma-rt),rl=strlen(comma+1);
            if(fl==0||fl>=sizeof(first)||rl==0||rl>=sizeof(rest)){
                log4c_error("id=%u route segment exceeds limit",p->id);ps_teardown(p);return -1;
            }
            memcpy(first,rt,fl); first[fl]=0; memcpy(rest,comma+1,rl+1);
        } else {
            size_t fl=strlen(rt);
            if(fl==0||fl>=sizeof(first)){log4c_error("id=%u route segment exceeds limit",p->id);ps_teardown(p);return -1;}
            memcpy(first,rt,fl+1);
        }

        if(strncmp(first,"T:",2)==0){
            nb_exit_recovery_state_t target_state=ps_exit_recovery_state(p);
            nb_exit_recovery_action_t target_action=nb_exit_node_target_route(&target_state,G.role);
            ps_exit_recovery_apply(p,&target_state);
            if(target_action==NB_EXIT_RECOVERY_ROLE_REJECT){
                log4c_warn("目标动作角色拒绝 id=%u role=%s action=T result=role-invalid",
                    p->id,role_name(G.role));
                ps_teardown_reason(p,NB_EXIT_TARGET_ROLE_CLOSE_REASON);return -1;
            }
            /* exit: 异步解析 target 域名(不阻塞事件循环), 解析完成后在主循环发起非阻塞 connect */
            char thost[256]={0}; int tport=0; char* c=strrchr(first+2,':');
            if(c){size_t host_len=(size_t)(c-(first+2));
                if(host_len>0&&host_len<sizeof(thost)&&parse_port_strict(c+1,&tport)==0){memcpy(thost,first+2,host_len);thost[host_len]=0;}}
            if(c==NULL||thost[0]==0||tport<=0||tport>65535){log4c_error("id=%u malformed target route",p->id);ps_teardown(p);return -1;}
            { nb_flow_policy_t target_pol; flow_policy_for_host(thost,tport,&target_pol); apply_flow_policy(p,&target_pol); }
            if(tport==NB_PROBE_PORT&&!strcasecmp(thost,NB_PROBE_SINK_HOST)){
                p->probe_mode=1;p->probe_hash=NB_PROBE_HASH_INITIAL;p->target_connect_state=2;
                log4c_info("exit internal probe start id=%u mode=sink",p->id);
            }else if(tport==NB_PROBE_PORT&&!strcasecmp(thost,NB_PROBE_ECHO_HOST)){
                p->probe_mode=2;p->target_connect_state=2;
            }else if(tport==NB_PROBE_PORT&&!strcasecmp(thost,NB_PROBE_SOURCE_HOST)){
                p->probe_mode=4;p->probe_hash=NB_PROBE_HASH_INITIAL;p->target_connect_state=2;
                log4c_info("exit internal probe start id=%u mode=downlink-source",p->id);
            }else if(!whitelist_allowed(thost,tport)){ /* 白名单外 -> 拒绝(reset stream), 出口访问控制 */
                log4c_debug("exit id=%u BLOCKED %s:%d (not in whitelist)",p->id,thost,tport);
                ps_teardown(p); return -1; }
            if(!p->probe_mode){
                if(dns_submit(p->id,thost,tport)!=0){ log4c_error("id=%u dns queue full %s:%d",p->id,thost,tport);
                    ps_teardown(p); return -1; }
                p->dns_pending=1; p->target_port=tport;
                log4c_debug("exit id=%u sid=%llu -> resolving %s:%d (async) prio=%d",p->id,(unsigned long long)p->up_stream_id,thost,tport,prio);
            }
        } else if(strncmp(first,"H:",2)==0){
            /* middle: 动态连下一跳 QUIC(地址来自首部), 转发剩余 route(带同一 prio)。 */
            char nhost[256]={0}; int nport=0; char* c=strrchr(first+2,':');
            if(c){size_t host_len=(size_t)(c-(first+2));
                if(host_len>0&&host_len<sizeof(nhost)&&parse_port_strict(c+1,&nport)==0){memcpy(nhost,first+2,host_len);nhost[host_len]=0;}}
            if(c==NULL||nhost[0]==0||nport<=0||nport>65535||rest[0]==0){log4c_error("id=%u malformed hop route",p->id);ps_teardown(p);return -1;}
            nport=worker_lane_port(nport);if(nport<1){ps_teardown_reason(p,"worker-lane-port");return -1;}
            struct sockaddr_storage naddr; int is_name=0;
            nb_flow_policy_t pol;
            char thost[256]; int tport=0;
            if(parse_target_from_route(rest[0]?rest:"", thost, sizeof(thost), &tport) == 0) flow_policy_for_host(thost, tport, &pol);
            else nb_flow_policy_default(&pol);
            if(!pol.matched) pol.prio = prio; /* 未命中 TikTok 规则时，回退使用上游携带的优先级 */
            if(picoquic_get_server_address(nhost,nport,&naddr,&is_name)!=0){
                log4c_error("id=%u resolve nexthop %s:%d fail",p->id,nhost,nport);
                ps_teardown(p); return -1; }
            p->down_pool_id=pool_get_or_create(&naddr);
            if(p->down_pool_id<0){ps_teardown(p);return -1;}
            if(downstream_open_stream(p, rest[0]?rest:"", &pol)!=0){ ps_teardown(p); return -1; }
            log4c_debug("middle id=%u up_sid=%llu -> nexthop %s:%d down_sid=%llu rest=[%s]",
                p->id,(unsigned long long)p->up_stream_id,nhost,nport,(unsigned long long)p->down_stream_id,rest);
        } else {
            char preview[512];
            fmt_route_preview(first, preview, sizeof(preview));
            log4c_error("id=%u bad route first-seg=[%s] hdr_len=%zu",p->id,preview,p->hdr_len);
            ps_teardown(p); return -1;
        }
    }
    if(off<len){
        if(G.role==ROLE_EXIT&&p->logical_endpoint){
            if(logical_receive(p,bytes+off,len-off)!=0){ps_teardown_reason(p,"logical-frame");return -1;}
        }else if(p->probe_mode){
            if(p->probe_mode==1){if(probe_sink_feed(p,bytes+off,len-off)!=0)return -1;}
            else if(p->probe_mode==4){if(probe_source_request(p,bytes+off,len-off)!=0)return -1;}
            else if(queue_up(p,bytes+off,len-off,0)!=0)return -1;
        }else fwd_down(p,bytes+off,len-off,0);
    }
    if(fin&&p->logical_mode)return 0;
    if(fin){ p->up_fin_seen=1;if(p->probe_mode)p->tcp_eof=1;
        if(p->probe_mode==1){
            if(!p->probe_ack_sent){
                char ack[128];int n=nb_probe_ack_format(p->probe_bytes,p->probe_hash,ack,sizeof(ack));
                if(n<0||queue_up(p,(const uint8_t*)ack,(size_t)n,1)!=0)return -1;
                p->probe_ack_sent=1;
            }
        }else if(p->probe_mode==2){
            if(queue_up(p,NULL,0,1)!=0)return -1;
        }else fwd_down(p,NULL,0,1);
    }
    return 0;
}

/* QUIC 回调: 统一处理 up(去程) / down(回程) 两侧 stream 事件 */
static int relay_quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t ev, void* cb_ctx, void* v_stream_ctx){
    (void)cb_ctx;
    proxy_stream_t* p;
    switch(ev){
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin: {
        int fin=(ev==picoquic_callback_stream_fin);
        /* 已绑定 stream 直接用 picoquic app ctx；仅首包或旧 peer 回退线性查找。 */
        p=(proxy_stream_t*)v_stream_ctx;
        if(p==NULL||!p->in_use||!((p->down_cnx==cnx&&p->down_stream_id==stream_id)||
            (p->fec_ctrl_cnx==cnx&&p->fec_ctrl_stream_id==stream_id)))p=ps_find_by_down(cnx,stream_id);
        if(p!=NULL){
            if(p->fec_ctrl_cnx==cnx && p->fec_ctrl_stream_id==stream_id){
                if(length>0) fec_ctrl_feed_bytes(p,bytes,length,1);
                break;
            }
            if(G.role==ROLE_ENTRY&&p->logical_endpoint){
                if(length>0&&logical_receive(p,bytes,length)!=0){ps_teardown_reason(p,"logical-frame");break;}
                if(fin)logical_detach(p,cnx);
            }else{
                if(length>0) fwd_up(p,bytes,length,0);
                if(fin){ p->down_fin_seen=1; if(G.role==ROLE_ENTRY) p->q2t_fin=1; else fwd_up(p,NULL,0,1); }
            }
            break;
        }
        if(G.role==ROLE_MIDDLE&&pool_contains_cnx(cnx)){
            log4c_warn("middle stale downstream stream sid=%llu ignored",(unsigned long long)stream_id);
            picoquic_discard_stream(cnx,stream_id,0);
            break;
        }
        /* 再按上游(去程)匹配 */
        p=(proxy_stream_t*)v_stream_ctx;
        if(p==NULL||!p->in_use||!(p->up_cnx==cnx&&p->up_stream_id==stream_id))p=ps_find_by_up(cnx,stream_id);
        if(p==NULL){
            /* middle/exit: 新上游 stream(server 端被动收) */
            if(G.role==ROLE_ENTRY){ break; } /* entry 无 server, 忽略 */
            if(transport_bind_connection(cnx,0)!=0){picoquic_discard_stream(cnx,stream_id,0);break;}
            p=ps_alloc(); if(!p){ log4c_warn("stream table full, reset sid=%llu",(unsigned long long)stream_id);
                picoquic_discard_stream(cnx,stream_id,0); break; }
            p->up_cnx=cnx; p->up_stream_id=stream_id;
            p->transport_generation=transport_connection_generation(cnx);
            ps_index_refresh(p);
            picoquic_set_app_stream_ctx(cnx,stream_id,p);
            if(G.role==ROLE_MIDDLE){
                int fc_ret=picoquic_set_app_flow_control(cnx,stream_id,1);
                if(fc_ret==0)p->upstream_fc_enabled=1;
                else log4c_warn("middle upstream fc enable failed id=%u sid=%llu ret=%d",
                    p->id,(unsigned long long)stream_id,fc_ret);
            }
        }
        on_up_data(&p,bytes,length,fin);
        break; }
    case picoquic_callback_prepare_to_send: {
        p=(proxy_stream_t*)v_stream_ctx;
        if(p==NULL || !p->in_use) break;
        if(p->down_cnx==cnx && p->down_stream_id==stream_id){
            size_t prefix_left=p->down_prefix_len-p->down_prefix_off;
            if(prefix_left>0){
                size_t nb=prefix_left<length?prefix_left:length;
                int still_active=(prefix_left>nb)||p->down_tx.len>0||p->down_tx_fin;
                uint8_t* dst=picoquic_provide_stream_data_buffer(bytes,nb,0,still_active);
                if(dst!=NULL&&nb>0){
                    if(p->down_prefix_prepare_at==0){
                        p->down_prefix_prepare_at=picoquic_current_time();
                        if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=route-prepare age=%.1fms queue_wait=%.1fms bytes=%zu allowance=%zu route=%s",
                            role_name(G.role),p->id,trace_delta_ms(p->down_prefix_prepare_at,p->created_at),
                            trace_delta_ms(p->down_prefix_prepare_at,p->down_prefix_queued_at),nb,length,p->route);
                    }
                    memcpy(dst,p->down_prefix+p->down_prefix_off,nb);
                    p->down_prefix_off+=nb;
                    if(p->down_prefix_off==p->down_prefix_len){
                        p->down_prefix_off=0;
                        p->down_prefix_len=0;
                        /* route 已送入 QUIC，后续 payload 恢复策略优先级，避免
                         * 新流首部长期抢占既有实时媒体数据。 */
                        picoquic_set_stream_priority(cnx,stream_id,(uint8_t)p->prio);
                    }
                    ps_touch(p);
                }else if(dst==NULL&&nb>0){
                    log4c_warn("id=%u provide down prefix fail sid=%llu",p->id,(unsigned long long)stream_id);
                }
                break;
            }
            size_t requested = (p->down_tx.len < length) ? p->down_tx.len : length;
            size_t nb=sched_stream_grant(p,cnx,requested,picoquic_current_time());
            if(requested>0&&nb==0){
                (void)picoquic_provide_stream_data_buffer(bytes,0,0,1);
                break;
            }
            int is_fin = (p->down_tx_fin && nb == p->down_tx.len);
            int still_active = (p->down_tx.len > nb) || (p->down_tx_fin && !is_fin);
            uint8_t* dst = picoquic_provide_stream_data_buffer(bytes, nb, is_fin, still_active);
            if(dst != NULL && nb > 0){
                if(p->first_down_prepare_at==0){
                    p->first_down_prepare_at=picoquic_current_time();
                    if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=c2s-prepare age=%.1fms queue_wait=%.1fms bytes=%zu q_before=%zu allowance=%zu route=%s",
                        role_name(G.role),p->id,trace_delta_ms(p->first_down_prepare_at,p->created_at),
                        trace_delta_ms(p->first_down_prepare_at,p->first_down_queue_at),nb,p->down_tx.len,length,p->route);
                }
                (void)nb_send_copyout(&p->down_tx,&p->queue_clocks->down_tx,dst,nb);
                middle_upstream_fc_update(p,picoquic_current_time());
                ps_touch(p);
            } else if(dst == NULL && (nb > 0 || is_fin)) {
                log4c_warn("id=%u provide down buffer fail sid=%llu", p->id, (unsigned long long)stream_id);
            }
            if(is_fin) p->down_tx_fin = 0;
        } else if(p->up_cnx==cnx && p->up_stream_id==stream_id){
            size_t requested = (p->up_tx.len < length) ? p->up_tx.len : length;
            size_t nb=sched_stream_grant(p,cnx,requested,picoquic_current_time());
            if(requested>0&&nb==0){
                (void)picoquic_provide_stream_data_buffer(bytes,0,0,1);
                break;
            }
            int is_fin = (p->up_tx_fin && nb == p->up_tx.len);
            int still_active = (p->up_tx.len > nb) || (p->up_tx_fin && !is_fin);
            uint8_t* dst = picoquic_provide_stream_data_buffer(bytes, nb, is_fin, still_active);
            if(dst != NULL && nb > 0){
                if(p->first_up_prepare_at==0){
                    p->first_up_prepare_at=picoquic_current_time();
                    if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=s2c-prepare age=%.1fms queue_wait=%.1fms bytes=%zu q_before=%zu allowance=%zu route=%s",
                        role_name(G.role),p->id,trace_delta_ms(p->first_up_prepare_at,p->created_at),
                        trace_delta_ms(p->first_up_prepare_at,p->first_up_queue_at),nb,p->up_tx.len,length,p->route);
                }
                (void)nb_send_copyout(&p->up_tx,&p->queue_clocks->up_tx,dst,nb);
                ps_touch(p);
            } else if(dst == NULL && (nb > 0 || is_fin)) {
                log4c_warn("id=%u provide up buffer fail sid=%llu", p->id, (unsigned long long)stream_id);
            }
            if(is_fin) p->up_tx_fin = 0;
        } else {
            (void)picoquic_provide_stream_data_buffer(bytes, 0, 0, 0);
        }
        break; }
    case picoquic_callback_prepare_datagram: {
        int handled = 0;
        uint32_t* datagram_rr=&nb_current_instance->datagram_rr;uint64_t now=picoquic_current_time();
        { nb_sched_cnx_t* runtime=runtime_for_cnx(cnx,0);
          if(runtime&&runtime->yfe2_control_length>0){
            size_t control_length=runtime->yfe2_control_length;
            if(control_length<=length){
                uint8_t* dst=picoquic_provide_datagram_buffer_ex(bytes,control_length,
                    picoquic_datagram_not_active);
                if(dst){memcpy(dst,runtime->yfe2_control_wire,control_length);
                    runtime->yfe2_control_length=0;handled=1;}
            }else{
                (void)picoquic_provide_datagram_buffer_ex(bytes,0,picoquic_datagram_not_active);
                handled=1;
            }
          } }
        { const uint8_t* control=NULL;size_t control_len=0,token=0;
          if(!handled&&nb_udp_lifecycle_next(&g_udp_lifecycle,(uintptr_t)cnx,now,&control,&control_len,&token)){
            if(control_len<=length){uint8_t* dst=picoquic_provide_datagram_buffer_ex(bytes,control_len,picoquic_datagram_not_active);if(dst!=NULL){memcpy(dst,control,control_len);nb_udp_lifecycle_sent(&g_udp_lifecycle,token,now);handled=1;}}
            else {(void)picoquic_provide_datagram_buffer_ex(bytes,0,picoquic_datagram_not_active);handled=1;} } }
        for(int media_pass=0;media_pass<3&&!handled;media_pass++){
          for(int scan=0;scan<MAX_CONN&&!handled;scan++){
            int i=(int)((*datagram_rr+(uint32_t)scan)%MAX_CONN);
            p=&G.streams[i];if(!p->in_use||!p->udp_mode||p->udp_association)continue;
            if(media_pass==0&&p->flow_class!=NB_FLOW_CLASS_MEDIA)continue;
            if(media_pass==2&&p->flow_class==NB_FLOW_CLASS_MEDIA)continue;
            uint8_t* queue=NULL;size_t* queue_len=NULL;
            if(media_pass==1){
                if(p->flow_class==NB_FLOW_CLASS_MEDIA&&p->down_cnx==cnx&&p->yfe2_parity_tx_len>=10){
                    queue=p->yfe2_parity_tx;queue_len=&p->yfe2_parity_tx_len;
                }else continue;
            }else if(p->down_cnx==cnx&&p->udp_down_tx_len>=10){
                queue=p->udp_down_tx;queue_len=&p->udp_down_tx_len;
            }else if(p->up_cnx==cnx&&p->udp_up_tx_len>=10){
                queue=p->udp_up_tx;queue_len=&p->udp_up_tx_len;
            }
            if(queue==NULL)continue;
            const nb_transport_runtime_t* runtime=p->down_cnx==cnx?
                &nb_current_instance->transport_egress:&nb_current_instance->transport_ingress;
            uint64_t reorder_delay_us=runtime->active?runtime->link.reorder_delay_us:
                nb_current_instance->reorder_delay_us;
            nb_live_queue_limits_t limits=nb_live_queue_limits_for_path(p->flow_class,1,reorder_delay_us);
            uint64_t dropped=nb_live_queue_expiry_enabled(p->flow_class,1)?
                dgramq_drop_expired(queue,queue_len,limits.deadline_us,now):0;
            p->yfe2_scheduler_expired+=dropped;
            if(dropped>0)log4c_info("%s udp deadline drop id=%u packets=%llu deadline=%.1fms target=%s:%d",
                role_name(G.role),p->id,(unsigned long long)dropped,limits.deadline_us/1000.0,
                p->udp_target_host,p->udp_target_port);
            const uint8_t* dg=NULL;size_t dg_len=0;uint64_t queued_at=0;
            if(!dgramq_peek(queue,*queue_len,&dg,&dg_len,&queued_at))continue;
            if(dg_len>length){
                if(dg_len>=NB_YFE2_WIRE_HEADER&&nb_get_u32(dg)==NB_UDP_FEC_MAGIC&&dg[4]==3){
                    int action=nb_yfe2_prepare_action(dg_len,length,
                        yfe2_max_datagram_payload(cnx));
                    if(action==NB_YFE2_PREPARE_FALLBACK){
                        dgramq_consume(queue,queue_len);
                        p->yfe2_pmtu_fallback++;p->yfe2_parity_queue_drop++;
                        nb_sched_cnx_t* yfe2_runtime=runtime_for_cnx(cnx,0);
                        if(yfe2_runtime)yfe2_runtime->yfe2_negotiation.state=
                            NB_YFE2_NEG_FALLBACK_PMTU;
                        log4c_info("%s NB YFE2 parity 因 PMTU 丢弃 id=%u wire=%zu max=%zu",
                            role_name(G.role),p->id,dg_len,yfe2_max_datagram_payload(cnx));
                    }
                }
                (void)picoquic_provide_datagram_buffer_ex(bytes,0,picoquic_datagram_active_any_path);
                handled=1;break;
            }
            int tenant_direction=-1;size_t tenant_charge=0;
            if(g_tenants_enabled&&((G.role==ROLE_ENTRY&&queue==p->udp_down_tx)||
                (G.role==ROLE_EXIT&&queue==p->udp_up_tx))){
                nb_udp_wire_view_t view;
                if(nb_udp_wire_decode(dg,dg_len,&view)==0){
                    tenant_direction=G.role==ROLE_ENTRY?NB_TENANT_UP:NB_TENANT_DOWN;
                    tenant_charge=view.payload_length;
                    size_t taken=nb_tenant_take_class(&g_tenants,p->tenant_index,tenant_charge,now,
                        tenant_direction,p->flow_class==NB_FLOW_CLASS_MEDIA&&!p->downlink_bulk);
                    if(taken<tenant_charge){
                        nb_tenant_refund(&g_tenants,p->tenant_index,taken,tenant_direction);
                        (void)picoquic_provide_datagram_buffer_ex(bytes,0,picoquic_datagram_active_any_path);
                        handled=1;break;
                    }
                }
            }
            uint8_t* dst=picoquic_provide_datagram_buffer_ex(bytes,dg_len,picoquic_datagram_active_any_path);
            if(dst!=NULL){
                if(queue==p->udp_down_tx&&p->udp_first_c2s_prepare_at==0){
                    p->udp_first_c2s_prepare_at=now;
                    log4c_info("%s udp trace id=%u usid=%u stage=c2s-first-prepare age=%.1fms queue_wait=%.1fms wire_bytes=%zu allowance=%zu target=%s:%d",
                        role_name(G.role),p->id,p->udp_session_id,trace_delta_ms(now,p->created_at),
                        trace_delta_ms(now,queued_at),dg_len,length,p->udp_target_host,p->udp_target_port);
                }else if(queue==p->udp_up_tx&&p->udp_first_s2c_prepare_at==0){
                    p->udp_first_s2c_prepare_at=now;
                    log4c_info("%s udp trace id=%u usid=%u stage=s2c-first-prepare age=%.1fms queue_wait=%.1fms wire_bytes=%zu allowance=%zu target=%s:%d",
                        role_name(G.role),p->id,p->udp_session_id,trace_delta_ms(now,p->created_at),
                        trace_delta_ms(now,queued_at),dg_len,length,p->udp_target_host,p->udp_target_port);
                }
                memcpy(dst,dg,dg_len);dgramq_consume(queue,queue_len);*datagram_rr=(uint32_t)(i+1);handled=1;
            }else if(tenant_charge>0)nb_tenant_refund(&g_tenants,p->tenant_index,tenant_charge,tenant_direction);
          }
        }
        for(int i=0;i<MAX_CONN&&!handled;i++){
            p = &G.streams[i];
            if(!p->in_use || p->fec_dg_cnx != cnx || p->fec_dg_tx_len < 10) continue;
            const uint8_t* dg = NULL; size_t dg_len = 0;
            if(!dgramq_peek(p->fec_dg_tx, p->fec_dg_tx_len, &dg, &dg_len,NULL)) continue;
            if(dg_len > length){
                (void)picoquic_provide_datagram_buffer_ex(bytes, 0, picoquic_datagram_active_any_path);
                handled = 1;
                break;
            }
            uint8_t* dst = picoquic_provide_datagram_buffer_ex(bytes, dg_len,
                (p->fec_dg_tx_len > (2 + dg_len)) ? picoquic_datagram_active_any_path : picoquic_datagram_not_active);
            if(dst != NULL){
                memcpy(dst, dg, dg_len);
                dgramq_consume(p->fec_dg_tx, &p->fec_dg_tx_len);
                p->fec_dg_inflight++;
                if(p->fec_dg_inflight>p->fec_dg_inflight_peak)p->fec_dg_inflight_peak=p->fec_dg_inflight;
                (void)fec_pump_staged(p);
                handled = 1;
                break;
            }
        }
        if(!handled){
            (void)picoquic_provide_datagram_buffer_ex(bytes, 0, picoquic_datagram_not_active);
        }
        break; }
    case picoquic_callback_datagram:
    case picoquic_callback_datagram_acked:
    case picoquic_callback_datagram_lost:
    case picoquic_callback_datagram_spurious: {
        if(length>=NB_YFE2_WIRE_HEADER&&nb_get_u32(bytes)==NB_UDP_FEC_MAGIC&&bytes[4]==3){
            if(ev==picoquic_callback_datagram)yfe2_receive_parity(cnx,bytes,length);
            break;
        }
        if(length>=24&&nb_get_u32(bytes)==NB_UDP_FEC_MAGIC&&bytes[4]==NB_UDP_FEC_VERSION){
            if(ev==picoquic_callback_datagram){
                uint32_t sid=0;uint8_t direction=0;
                if(nb_udp_fec_peek(bytes,length,&sid,&direction)==0){
                    p=G.role==ROLE_EXIT?ps_find_udp_up(cnx,sid):
                        (G.role==ROLE_ENTRY?ps_find_udp_down(cnx,sid):NULL);
                    if(p!=NULL&&p->flow_class==NB_FLOW_CLASS_MEDIA){
                        if(p->udp_fec_rx==NULL)p->udp_fec_rx=nb_udp_fec_rx_create();
                        if(p->udp_fec_rx!=NULL){
                            uint8_t recovered[NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32];
                            p->udp_fec_repairs_received++;
                            for(;;){
                                int recovered_len=nb_udp_fec_rx_recover(p->udp_fec_rx,bytes,length,
                                    picoquic_current_time(),recovered,sizeof(recovered));
                                if(recovered_len<=0){
                                    if(recovered_len<0)log4c_warn("%s udp FEC repair rejected usid=%u length=%zu",
                                        role_name(G.role),sid,length);
                                    break;
                                }
                                nb_udp_wire_view_t view;
                                if(nb_udp_wire_decode(recovered,(size_t)recovered_len,&view)==0&&
                                    udp_on_quic_datagram(cnx,&view)==0){
                                    p->udp_fec_recovered++;
                                    log4c_info("%s udp FEC recovered id=%u usid=%u seq=%u frag=%u/%u",
                                        role_name(G.role),p->id,p->udp_session_id,view.sequence,
                                        view.fragment_index+1,view.fragment_count);
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        if(length>=24&&nb_get_u32(bytes)==NB_UDP_MAGIC){
            if(ev==picoquic_callback_datagram){
                nb_udp_wire_view_t view;
                int decode_rc=nb_udp_wire_decode(bytes,length,&view);
                if(decode_rc!=0)log4c_warn("%s udp datagram reject stage=decode length=%zu",
                    role_name(G.role),length);
                else {
                    p=G.role==ROLE_EXIT?ps_find_udp_up(cnx,view.session_id):
                        (G.role==ROLE_ENTRY?ps_find_udp_down(cnx,view.session_id):NULL);
                    if(p!=NULL&&p->udp_fec_rx!=NULL&&nb_udp_fec_rx_seen(p->udp_fec_rx,&view))break;
                    int yfe2_source_noted=0;
                    nb_sched_cnx_t* yfe2_runtime=runtime_for_cnx(cnx,0);
                    if(p!=NULL&&p->yfe2_rx!=NULL&&yfe2_runtime&&
                        yfe2_runtime->yfe2_negotiation.state==NB_YFE2_NEG_ACCEPTED){
                        int note_result=nb_yfe2_rx_note_source(p->yfe2_rx,
                            yfe2_runtime->yfe2_generation,&view,picoquic_current_time());
                        if(note_result>0)break;
                        yfe2_source_noted=note_result==0;
                    }
                    int handler_rc=udp_on_quic_datagram(cnx,&view);
                    if(handler_rc!=0)log4c_warn("%s udp datagram reject stage=handler rc=%d length=%zu type=%u usid=%u seq=%u frag=%u/%u route_len=%u",
                        role_name(G.role),handler_rc,length,view.type,view.session_id,view.sequence,
                        view.fragment_index+1,view.fragment_count,view.route_length);
                    else {
                        p=G.role==ROLE_EXIT?ps_find_udp_up(cnx,view.session_id):
                            (G.role==ROLE_ENTRY?ps_find_udp_down(cnx,view.session_id):NULL);
                        if(p!=NULL&&p->flow_class==NB_FLOW_CLASS_MEDIA){
                            if(p->udp_fec_rx==NULL)p->udp_fec_rx=nb_udp_fec_rx_create();
                            if(p->udp_fec_rx!=NULL)(void)nb_udp_fec_rx_note(p->udp_fec_rx,&view,picoquic_current_time());
                            yfe2_runtime=runtime_for_cnx(cnx,0);
                            if(!yfe2_source_noted&&G.role==ROLE_EXIT&&yfe2_runtime&&
                                yfe2_runtime->yfe2_negotiation.state==NB_YFE2_NEG_ACCEPTED&&!p->probe_mode){
                                if(p->yfe2_rx==NULL)p->yfe2_rx=nb_yfe2_rx_create();
                                if(p->yfe2_rx)(void)nb_yfe2_rx_note_source(p->yfe2_rx,
                                    yfe2_runtime->yfe2_generation,&view,picoquic_current_time());
                            }
                        }
                    }
                }
            }
            break;
        }
        if(length>=20 && nb_get_u32(bytes)==NB_FEC_DGRAM_MAGIC && bytes[4]==NB_FEC_PROTOCOL_VERSION &&
            (bytes[5]==nb_fec_dgram_source || bytes[5]==nb_fec_dgram_repair)){
            uint32_t sid=nb_get_u32(bytes+6);
            p=ps_find_by_fec_session(sid);
            if(p!=NULL&&p->fec_engine!=NULL){
                if(ev==picoquic_callback_datagram){
                    uint32_t block_id=nb_get_u32(bytes+10); uint8_t symbol_idx=bytes[14];
                    p->fec_dg_recv++;
                    if(!fec_should_drop(p,bytes[5],block_id,symbol_idx)){
                        int rc=nb_fec_on_datagram(p->fec_engine,bytes,length,picoquic_current_time());
                        if(rc!=NB_FEC_OK){ log4c_warn("%s fec engine datagram fail sid=%u rc=%d",role_name(G.role),sid,rc); p->need_teardown=1; }
                    }
                }else if(ev==picoquic_callback_datagram_acked){
                    p->fec_dg_acked++;if(p->fec_dg_inflight)p->fec_dg_inflight--;(void)fec_pump_staged(p);
                }else if(ev==picoquic_callback_datagram_lost){
                    p->fec_dg_lost++;if(p->fec_dg_inflight)p->fec_dg_inflight--;(void)fec_pump_staged(p);
                }
            }
            break;
        }
        if(length >= 14 && nb_get_u32(bytes) == NB_FEC_DGRAM_MAGIC && bytes[4] == NB_FEC_DGRAM_VER){
            uint8_t dgt = bytes[5];
            uint32_t sid = nb_get_u32(bytes + 6);
            uint32_t seq = (length >= 14) ? nb_get_u32(bytes + 10) : 0;
            p = ps_find_by_fec_session(sid);
            if(p != NULL){
                if(ev == picoquic_callback_datagram){
                    p->fec_dg_recv++;
                    log4c_debug("%s fec dgram recv sid=%u type=%u seq=%u", role_name(G.role), sid, dgt, seq);
                    if(G.role == ROLE_EXIT && dgt == nb_fec_dgram_hello){
                        p->fec_dg_cnx = cnx;
                        p->fec_ctrl_peer_ready = 1;
                        if(!p->need_teardown) (void)fec_pump_staged(p);
                        (void)fec_queue_probe_datagram(p, nb_fec_dgram_ack);
                    } else if(G.role == ROLE_MIDDLE && dgt == nb_fec_dgram_ack){
                        p->fec_ctrl_peer_ready = 1;
                        if(!p->need_teardown) (void)fec_pump_staged(p);
                    }
                } else if(ev == picoquic_callback_datagram_acked){
                    p->fec_dg_acked++;if(p->fec_dg_inflight)p->fec_dg_inflight--;(void)fec_pump_staged(p);
                } else if(ev == picoquic_callback_datagram_lost){
                    p->fec_dg_lost++;if(p->fec_dg_inflight)p->fec_dg_inflight--;(void)fec_pump_staged(p);
                }
            }
        }
        break; }
    case picoquic_callback_stream_reset:
    case picoquic_callback_stop_sending:
        /* 对端 reset/stop 单条 stream -> 主动 teardown, discard 另一侧 stream 连锁通知(middle 据此回收) */
        if((p=ps_find_by_down(cnx,stream_id))!=NULL){
            if(ev==picoquic_callback_stop_sending&&
                nb_lstream_stop_requires_reset_wait(G.role==ROLE_ENTRY,p->logical_endpoint)){
                /* discard_stream sends STOP_SENDING before RESET_STREAM. The
                 * restart reason is only observable on the reset callback. */
                log4c_debug("entry logical flow stop deferred id=%u sid=%llu",
                    p->id,(unsigned long long)stream_id);
                break;
            }
            uint64_t stream_error=picoquic_get_remote_stream_error(cnx,stream_id);
            if(nb_lstream_reset_requires_resume(G.role==ROLE_ENTRY,p->logical_endpoint,
                stream_error)){
                const char* reason=ev==picoquic_callback_stream_reset?
                    "quic-stream-reset":"quic-stop-sending";
                picoquic_set_app_stream_ctx(cnx,stream_id,NULL);
                logical_detach(p,cnx);
                (void)picoquic_discard_stream(cnx,stream_id,0);
                log4c_warn("entry logical flow detached id=%u reason=%s; resume scheduled",
                    p->id,reason);
            }else ps_teardown_reason(p,ev==picoquic_callback_stream_reset?
                "quic-stream-reset":"quic-stop-sending"); }
        else if((p=ps_find_by_up(cnx,stream_id))!=NULL){
            ps_teardown_reason(p,ev==picoquic_callback_stream_reset?"quic-stream-reset":"quic-stop-sending"); }
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset: {
        const char* close_reason=ev==picoquic_callback_close?"quic-connection-close":
            (ev==picoquic_callback_application_close?"quic-application-close":"quic-stateless-reset");
        int i; for(i=0;i<MAX_CONN;i++) if(G.streams[i].in_use && (G.streams[i].up_cnx==cnx||G.streams[i].down_cnx==cnx||G.streams[i].fec_ctrl_cnx==cnx)){
            proxy_stream_t* affected=&G.streams[i];
            if(affected->logical_endpoint&&((G.role==ROLE_ENTRY&&affected->down_cnx==cnx)||
                (G.role==ROLE_EXIT&&affected->up_cnx==cnx))){
                log4c_warn("%s logical flow detached id=%u reason=%s",role_name(G.role),affected->id,close_reason);
                logical_detach(affected,cnx);
            }else if(G.role==ROLE_MIDDLE&&affected->logical_mode&&affected->down_cnx==cnx&&
                affected->up_cnx!=NULL){
                /* Middle does not own end-to-end replay state. Reset the live
                 * upstream stream so Entry preserves its TCP socket and opens
                 * a new route+RESUME stream through the replacement link. */
                picoquic_cnx_t* upstream=affected->up_cnx;
                uint64_t upstream_sid=affected->up_stream_id;
                picoquic_set_app_stream_ctx(upstream,upstream_sid,NULL);
                (void)picoquic_discard_stream(upstream,upstream_sid,
                    NB_LSTREAM_RELAY_RESTART_ERROR);
                ps_set_close_reason(affected,"logical-relay-downstream-close");
                log4c_warn("middle logical relay restart id=%u reason=%s upstream_sid=%llu",
                    affected->id,close_reason,(unsigned long long)upstream_sid);
                ps_free(affected);
            }else{ps_set_close_reason(affected,close_reason);ps_free(affected);}
        }
        for(int pool_id=0;pool_id<G.pool_count;pool_id++)for(i=0;i<POOL_SIZE;i++){
            cnx_pool_t* pool=&G.pools[pool_id];
            if(pool->cnx[i]==cnx){
                picoquic_cnx_t* replacement=pool->replacement_cnx[i];
                if(replacement&&!pool->replacement_closing[i]){
                    pool->cnx[i]=replacement;pool->replacement_cnx[i]=NULL;
                    pool->replacement_closing[i]=0;pool->next_sid[i]=0;
                    g_metrics_state.pool_mbb_promotions++;
                    log4c_warn("pool[%d:%d] active connection closed; replacement promoted",pool_id,i);
                }else pool->cnx[i]=NULL;
                nb_pool_slot_reset_quality(pool,i);memset(&pool->health[i],0,sizeof(pool->health[i]));
            }
            if(pool->replacement_cnx[i]==cnx){
                pool->replacement_cnx[i]=NULL;pool->replacement_closing[i]=0;
            }
            if(pool->draining_cnx[i]==cnx){
                pool->draining_cnx[i]=NULL;pool->draining_closing[i]=0;
            }
        }
        udp_lifecycle_forget_cnx(cnx);sched_forget_cnx(cnx);picoquic_set_callback(cnx,NULL,NULL); break; }
    default: break;
    }
    return 0;
}

/* entry: 建到第一跳的 QUIC 连接池(POOL_SIZE 条并行连接) */
