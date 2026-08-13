static proxy_stream_t* ps_alloc(void){
    if(g_ps_inuse>=(int)nb_current_instance->session_limit)return NULL;
    int i; for(i=0;i<MAX_CONN;i++) if(!G.streams[i].in_use){
        memset(&G.streams[i],0,sizeof(G.streams[i]));
        G.streams[i].queue_clocks=calloc(1,sizeof(*G.streams[i].queue_clocks));
        if(G.streams[i].queue_clocks==NULL)return NULL;
        nb_ring_bind_budget(&G.streams[i].down_tx,&nb_current_instance->queue_bytes,nb_current_instance->queue_limit_bytes);
        nb_ring_bind_budget(&G.streams[i].up_tx,&nb_current_instance->queue_bytes,nb_current_instance->queue_limit_bytes);
        nb_ring_bind_budget(&G.streams[i].q2t,&nb_current_instance->queue_bytes,nb_current_instance->queue_limit_bytes);
        G.streams[i].in_use=1; G.streams[i].tcp_fd=-1; G.streams[i].udp_fd=-1; G.streams[i].id=++G.next_ps_id;
        G.streams[i].down_pool_idx=-1;
        G.streams[i].down_pool_id=-1;
        G.streams[i].tenant_index=-1;
        G.streams[i].route_index=-1;
        G.streams[i].last_active=picoquic_current_time();
        G.streams[i].created_at=G.streams[i].last_active;
        G.streams[i].transport_generation=nb_current_instance->transport_profiles.has_active?
            nb_current_instance->transport_profiles.active.generation:0;
        (void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_ID,G.streams[i].id,0,0,&G.streams[i]);
        g_ps_inuse++; if(g_ps_inuse>g_ps_peak) g_ps_peak=g_ps_inuse;
        return &G.streams[i]; }
    return NULL;
}
static void ps_touch(proxy_stream_t* p){ p->last_active=picoquic_current_time(); }
static void instance_queue_release(size_t length){
    if((uint64_t)length>=nb_current_instance->queue_bytes)nb_current_instance->queue_bytes=0;
    else nb_current_instance->queue_bytes-=(uint64_t)length;
}
static void ps_index_refresh(proxy_stream_t* p){
    if(p==NULL||!p->in_use)return;
    (void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_ID,p->id,0,0,p);
    if(p->up_cnx){
        (void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_UP_STREAM,(uintptr_t)p->up_cnx,p->up_stream_id,0,p);
        if(p->udp_mode&&!p->udp_association)(void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_UDP_UP,(uintptr_t)p->up_cnx,p->udp_session_id,0,p);
    }
    if(p->down_cnx){
        (void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_DOWN_STREAM,(uintptr_t)p->down_cnx,p->down_stream_id,0,p);
        if(p->udp_mode&&!p->udp_association)(void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_UDP_DOWN,(uintptr_t)p->down_cnx,p->udp_session_id,0,p);
    }
    if(p->fec_ctrl_cnx)(void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_DOWN_STREAM,(uintptr_t)p->fec_ctrl_cnx,p->fec_ctrl_stream_id,0,p);
    if(p->udp_parent_id&&p->udp_target_host[0]){
        (void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_UDP_CHILD,p->udp_parent_id,
            nb_session_index_hash_text(p->udp_target_host),(uint64_t)(unsigned)p->udp_target_port,p);
    }
}
static proxy_stream_t* ps_find_by_fec_session(uint32_t sid){
    proxy_stream_t* p=fec_session_index_find(sid);
    return p!=NULL&&p->in_use?p:NULL;
}
static proxy_stream_t* ps_find_by_up(picoquic_cnx_t* cnx, uint64_t sid){
    proxy_stream_t* indexed=nb_session_index_find(&g_session_index,NB_SESSION_KEY_UP_STREAM,(uintptr_t)cnx,sid,0);
    if(indexed&&indexed->in_use&&indexed->up_cnx==cnx&&indexed->up_stream_id==sid)return indexed;
    int i; for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
        if(p->in_use && p->up_cnx==cnx && p->up_stream_id==sid){ps_index_refresh(p);return p;} }
    return NULL;
}
static proxy_stream_t* ps_find_by_down(picoquic_cnx_t* cnx, uint64_t sid){
    proxy_stream_t* indexed=nb_session_index_find(&g_session_index,NB_SESSION_KEY_DOWN_STREAM,(uintptr_t)cnx,sid,0);
    if(indexed&&indexed->in_use&&((indexed->down_cnx==cnx&&indexed->down_stream_id==sid)||
        (indexed->fec_ctrl_cnx==cnx&&indexed->fec_ctrl_stream_id==sid)))return indexed;
    int i; for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
        if(p->in_use && ((p->down_cnx==cnx && p->down_stream_id==sid) ||
            (p->fec_ctrl_cnx==cnx && p->fec_ctrl_stream_id==sid))){ps_index_refresh(p);return p;} }
    return NULL;
}
static proxy_stream_t* ps_find_by_id(uint32_t id){
    proxy_stream_t* indexed=nb_session_index_find(&g_session_index,NB_SESSION_KEY_ID,id,0,0);
    if(indexed&&indexed->in_use&&indexed->id==id)return indexed;
    int i; for(i=0;i<MAX_CONN;i++) if(G.streams[i].in_use && G.streams[i].id==id){ps_index_refresh(&G.streams[i]);return &G.streams[i];}
    return NULL;
}

static proxy_stream_t* ps_find_logical(uint64_t hi,uint64_t lo,const proxy_stream_t* exclude){
    if(hi==0&&lo==0)return NULL;
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p!=exclude&&p->in_use&&p->logical_endpoint&&p->logical_flow_hi==hi&&p->logical_flow_lo==lo)return p;}
    return NULL;
}

static int logical_endpoint_init(proxy_stream_t* p,uint64_t hi,uint64_t lo){
    if(p==NULL||p->udp_mode||(hi==0&&lo==0))return -1;
    if(p->logical_decoder==NULL){
        p->logical_decoder=calloc(1,sizeof(*p->logical_decoder));
        if(p->logical_decoder==NULL)return -1;
    }
    p->logical_mode=1;p->logical_endpoint=1;p->logical_flow_hi=hi;p->logical_flow_lo=lo;
    nb_lstream_tx_init(&p->logical_tx,hi,lo);
    nb_lstream_tx_bind_budget(&p->logical_tx,&nb_current_instance->queue_bytes,
        nb_current_instance->queue_limit_bytes);
    nb_lstream_decoder_init(p->logical_decoder);return 0;
}

static int udp_reassembly_ensure(proxy_stream_t* p){
    if(p==NULL)return -1;
    if(p->udp_reassembly!=NULL)return 0;
    p->udp_reassembly=calloc(1,sizeof(*p->udp_reassembly));
    if(p->udp_reassembly==NULL)return -1;
    nb_udp_reassembly_init(p->udp_reassembly);return 0;
}

static uint64_t ps_dynamic_capacity(const proxy_stream_t* p){
    if(p==NULL)return 0;
    uint64_t total=p->down_tx.cap+p->up_tx.cap+p->q2t.cap+p->logical_tx.replay_capacity+
        p->fec_dg_tx_cap+p->fec_stage_tx_cap+p->udp_down_tx_cap+p->udp_up_tx_cap+p->udp_pending_tx_cap;
    if(p->queue_clocks)total+=sizeof(*p->queue_clocks);
    if(p->logical_decoder)total+=sizeof(*p->logical_decoder);
    if(p->fec_ctrl_rxbuf)total+=NB_FEC_CTRL_RX_CAP;
    if(p->udp_reassembly)total+=sizeof(*p->udp_reassembly);
    return total;
}

static int logical_entry_init(proxy_stream_t* p){
    if(p==NULL)return -1;
    if(p->logical_endpoint)return 0;
    uint64_t ids[2]={0,0};
    for(int attempt=0;attempt<4&&ids[0]==0&&ids[1]==0;attempt++)picoquic_crypto_random(G.quic,ids,sizeof(ids));
    return logical_endpoint_init(p,ids[0],ids[1]);
}

static void ps_release_rebind_shell(proxy_stream_t* shell){
    if(shell==NULL||!shell->in_use)return;
    g_memory_released_pending+=ps_dynamic_capacity(shell);
    nb_session_index_remove_value(&g_session_index,shell);
    nb_ring_dispose(&shell->down_tx);nb_ring_dispose(&shell->up_tx);nb_ring_dispose(&shell->q2t);
    instance_queue_release(shell->fec_dg_tx_len+shell->fec_stage_tx_len+shell->udp_down_tx_len+
        shell->udp_up_tx_len+shell->udp_pending_tx_len);
    free(shell->fec_dg_tx);free(shell->fec_stage_tx);free(shell->udp_down_tx);
    free(shell->udp_up_tx);free(shell->udp_pending_tx);
    nb_lstream_tx_dispose(&shell->logical_tx);
    free(shell->queue_clocks);shell->queue_clocks=NULL;
    free(shell->logical_decoder);shell->logical_decoder=NULL;
    free(shell->fec_ctrl_rxbuf);shell->fec_ctrl_rxbuf=NULL;
    if(shell->udp_reassembly){nb_udp_reassembly_dispose(shell->udp_reassembly);free(shell->udp_reassembly);shell->udp_reassembly=NULL;}
    if(g_ps_inuse>0)g_ps_inuse--;
    memset(shell,0,sizeof(*shell));
}

static proxy_stream_t* logical_rebind_upstream(proxy_stream_t* shell,proxy_stream_t* existing){
    if(shell==NULL||existing==NULL||shell==existing)return existing;
    picoquic_cnx_t* cnx=shell->up_cnx;uint64_t sid=shell->up_stream_id;
    picoquic_cnx_t* old_cnx=existing->up_cnx;uint64_t old_sid=existing->up_stream_id;
    nb_session_index_remove_value(&g_session_index,existing);
    nb_send_clear(&existing->up_tx,&existing->queue_clocks->up_tx);
    nb_send_clear(&existing->q2t,&existing->queue_clocks->q2t);existing->logical_rx_next=existing->logical_rx_delivered;
    if(!existing->logical_rx_fin_acked_sent){existing->logical_rx_fin_seen=0;existing->logical_rx_fin_offset=0;existing->q2t_fin=0;}
    if(existing->logical_decoder)nb_lstream_decoder_init(existing->logical_decoder);
    existing->up_cnx=cnx;existing->up_stream_id=sid;
    existing->logical_reconnect_pending=0;existing->logical_disconnected_at=0;
    existing->logical_reconnect_last_attempt=0;existing->logical_resume_inflight=0;
    existing->logical_reconnect_failures=0;
    picoquic_set_app_stream_ctx(cnx,sid,existing);
    if(old_cnx&&old_cnx!=cnx){picoquic_set_app_stream_ctx(old_cnx,old_sid,NULL);picoquic_discard_stream(old_cnx,old_sid,0);}
    ps_release_rebind_shell(shell);ps_index_refresh(existing);return existing;
}

static proxy_stream_t* ps_find_udp_up(picoquic_cnx_t* cnx,uint32_t session_id){
    proxy_stream_t* indexed=nb_session_index_find(&g_session_index,NB_SESSION_KEY_UDP_UP,(uintptr_t)cnx,session_id,0);
    if(indexed&&indexed->in_use&&indexed->udp_mode&&indexed->udp_session_id==session_id&&indexed->up_cnx==cnx)return indexed;
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p->in_use&&p->udp_mode&&!p->udp_association&&p->udp_session_id==session_id&&p->up_cnx==cnx){ps_index_refresh(p);return p;}}
    return NULL;
}

static proxy_stream_t* ps_find_udp_down(picoquic_cnx_t* cnx,uint32_t session_id){
    proxy_stream_t* indexed=nb_session_index_find(&g_session_index,NB_SESSION_KEY_UDP_DOWN,(uintptr_t)cnx,session_id,0);
    if(indexed&&indexed->in_use&&indexed->udp_mode&&indexed->udp_session_id==session_id&&indexed->down_cnx==cnx)return indexed;
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p->in_use&&p->udp_mode&&!p->udp_association&&p->udp_session_id==session_id&&p->down_cnx==cnx){ps_index_refresh(p);return p;}}
    return NULL;
}

static proxy_stream_t* ps_find_udp_child(uint32_t parent_id,const char* host,int port){
    proxy_stream_t* indexed=nb_session_index_find(&g_session_index,NB_SESSION_KEY_UDP_CHILD,parent_id,
        nb_session_index_hash_text(host),(uint64_t)(unsigned)port);
    if(indexed&&indexed->in_use&&indexed->udp_parent_id==parent_id&&indexed->udp_target_port==port&&!strcasecmp(indexed->udp_target_host,host))return indexed;
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p->in_use&&p->udp_mode&&!p->udp_association&&p->udp_parent_id==parent_id&&
            p->udp_target_port==port&&!strcasecmp(p->udp_target_host,host)){ps_index_refresh(p);return p;}}
    return NULL;
}

static uint32_t udp_session_id_new(void){
    for(uint32_t attempt=0;attempt<MAX_CONN*2u;attempt++){
        uint32_t sid=(uint32_t)picoquic_current_time()^(G.next_ps_id*2654435761u)^attempt;
        if(sid==0)continue;
        int used=0;
        for(int i=0;i<MAX_CONN;i++)if(G.streams[i].in_use&&G.streams[i].udp_session_id==sid){used=1;break;}
        if(!used)return sid;
    }
    return 0;
}

static int sockaddr_ip_equal(const struct sockaddr_storage* a,const struct sockaddr_storage* b){
    if(a==NULL||b==NULL||a->ss_family!=b->ss_family)return 0;
    if(a->ss_family==AF_INET)return ((const struct sockaddr_in*)a)->sin_addr.s_addr==((const struct sockaddr_in*)b)->sin_addr.s_addr;
    if(a->ss_family==AF_INET6)return memcmp(&((const struct sockaddr_in6*)a)->sin6_addr,&((const struct sockaddr_in6*)b)->sin6_addr,16)==0;
    return 0;
}
static void ps_clear_stream_ctx_once(picoquic_cnx_t* cnx, uint64_t sid,
    picoquic_cnx_t** seen_cnx, uint64_t* seen_sid, int* seen_n){
    if(cnx == NULL || seen_cnx == NULL || seen_sid == NULL || seen_n == NULL) return;
    for(int i=0;i<*seen_n;i++){
        if(seen_cnx[i] == cnx && seen_sid[i] == sid) return;
    }
    picoquic_set_app_stream_ctx(cnx, sid, NULL);
    if(*seen_n < 8){
        seen_cnx[*seen_n] = cnx;
        seen_sid[*seen_n] = sid;
        (*seen_n)++;
    }
}
static void ps_free(proxy_stream_t* p){
    if(!p->in_use) return;               /* 幂等: 防拆流/回调重入导致 double free */
    g_memory_released_pending+=ps_dynamic_capacity(p);
    if(p->close_reason[0] == 0) ps_set_close_reason(p, "released");
    if(p->logical_endpoint)log4c_info("%s logical flow close id=%u flow=%016llx%016llx reason=%s tx_base=%llu tx_next=%llu replay=%zu rx_delivered=%llu rx_next=%llu",
        role_name(G.role),p->id,(unsigned long long)p->logical_flow_hi,(unsigned long long)p->logical_flow_lo,p->close_reason,
        (unsigned long long)p->logical_tx.base_offset,(unsigned long long)p->logical_tx.next_offset,p->logical_tx.replay_length,
        (unsigned long long)p->logical_rx_delivered,(unsigned long long)p->logical_rx_next);
    if(p->udp_mode&&!p->udp_association&&!p->udp_close_received){if(G.role!=ROLE_EXIT)(void)udp_lifecycle_schedule(p->down_cnx,NB_UDP_TYPE_CLOSE,p->udp_session_id,p->route);if(G.role!=ROLE_ENTRY)(void)udp_lifecycle_schedule(p->up_cnx,NB_UDP_TYPE_CLOSE,p->udp_session_id,p->route);}
    if(g_tenants_enabled&&p->tenant_index>=0){nb_tenant_account(&g_tenants,p->tenant_index,p->bytes_c2s-p->tenant_accounted_up,p->bytes_s2c-p->tenant_accounted_down);if(p->tenant_acquired)nb_tenant_release(&g_tenants,p->tenant_index,p->tenant_udp);}
    /* Business-session lifetime is not a route-health signal. Active probes own health updates. */
    if(G.role==ROLE_ENTRY&&p->route_index>=0)nb_routes_finish_session(&G.exit_routes,(size_t)p->route_index);
    nb_metrics_note_traffic(&g_metrics_state,p->bytes_c2s,p->bytes_s2c);
    nb_metrics_note_close(&g_metrics_state,p->close_reason);
    if(p->udp_mode && !p->udp_association){
        log4c_info("%s udp flow close id=%u usid=%u reason=%s age=%.1fms packets=%llu/%llu bytes=%llu/%llu pressure_drop=%llu milestones_ms=local_c2s:%.1f,c2s_rx:%.1f,c2s_prepare:%.1f,target_ready:%.1f,target_send:%.1f,target_rx:%.1f,s2c_rx:%.1f,s2c_prepare:%.1f,local_s2c:%.1f target=%s:%d route=%s",
            role_name(G.role),p->id,p->udp_session_id,p->close_reason,
            (picoquic_current_time()-p->created_at)/1000.0,
            (unsigned long long)p->udp_packets_c2s,(unsigned long long)p->udp_packets_s2c,
            (unsigned long long)p->bytes_c2s,(unsigned long long)p->bytes_s2c,
            (unsigned long long)p->udp_queue_pressure_dropped,
            trace_delta_ms(p->udp_first_local_c2s_at,p->created_at),
            trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),
            trace_delta_ms(p->udp_first_c2s_prepare_at,p->created_at),
            trace_delta_ms(p->udp_target_ready_at,p->created_at),
            trace_delta_ms(p->udp_first_target_send_at,p->created_at),
            trace_delta_ms(p->udp_first_target_rx_at,p->created_at),
            trace_delta_ms(p->udp_first_s2c_rx_at,p->created_at),
            trace_delta_ms(p->udp_first_s2c_prepare_at,p->created_at),
            trace_delta_ms(p->udp_first_local_s2c_at,p->created_at),
            p->udp_target_host,p->udp_target_port,p->route);
    }
    if(p->udp_association){
        log4c_info("entry UDP ASSOCIATE close id=%u reason=%s age=%.1fms raw_rx=%llu reject_ip=%llu malformed=%llu policy_drop=%llu peer_learned=%d peer=%s",
            p->id,p->close_reason,(picoquic_current_time()-p->created_at)/1000.0,
            (unsigned long long)p->udp_assoc_raw_rx,(unsigned long long)p->udp_assoc_reject_ip,
            (unsigned long long)p->udp_assoc_malformed,(unsigned long long)p->udp_assoc_policy_drop,
            p->udp_client_peer_set,p->peer_addr);
        uint32_t parent_id=p->id;
        for(int i=0;i<MAX_CONN;i++) if(G.streams[i].in_use&&G.streams[i].udp_parent_id==parent_id){
            ps_set_close_reason(&G.streams[i],"udp-association-close");ps_free(&G.streams[i]);
        }
    }
    if(ps_is_rtc_webcast(p)){
        uint64_t now = picoquic_current_time();
        double first_c2s_ms = p->first_c2s_at ? (p->first_c2s_at - p->created_at) / 1000.0 : -1.0;
        double first_s2c_ms = p->first_s2c_at ? (p->first_s2c_at - p->created_at) / 1000.0 : -1.0;
        double connect_ms = p->target_connect_at && p->target_connect_done_at
            ? (p->target_connect_done_at - p->target_connect_at) / 1000.0 : -1.0;
        log4c_info("%s media flow close id=%u reason=%s age=%.1fms c2s=%llu s2c=%llu first_c2s=%.1fms first_s2c=%.1fms target_connect=%s connect_ms=%.1f route=%s",
            role_name(G.role), p->id, p->close_reason, (now - p->created_at) / 1000.0,
            (unsigned long long)p->bytes_c2s, (unsigned long long)p->bytes_s2c,
            first_c2s_ms, first_s2c_ms, ps_connect_state_name(p->target_connect_state),
            connect_ms, p->route);
    }
    log4c_debug("ps free id=%u up_sid=%llu down_sid=%llu route=%s", p->id,
        (unsigned long long)p->up_stream_id, (unsigned long long)p->down_stream_id, p->route);
    picoquic_cnx_t* seen_cnx[8];
    uint64_t seen_sid[8];
    int seen_n = 0;
    if(p->up_cnx) ps_clear_stream_ctx_once(p->up_cnx, p->up_stream_id, seen_cnx, seen_sid, &seen_n);
    if(p->down_cnx && p->down_opened) ps_clear_stream_ctx_once(p->down_cnx, p->down_stream_id, seen_cnx, seen_sid, &seen_n);
    if(p->fec_ctrl_cnx && p->fec_ctrl_opened) ps_clear_stream_ctx_once(p->fec_ctrl_cnx, p->fec_ctrl_stream_id, seen_cnx, seen_sid, &seen_n);
    if(p->tcp_fd>=0){ if(G.epoll_fd>=0)epoll_ctl(G.epoll_fd,EPOLL_CTL_DEL,p->tcp_fd,NULL); close(p->tcp_fd); }
    if(p->udp_fd>=0){ if(G.epoll_fd>=0)epoll_ctl(G.epoll_fd,EPOLL_CTL_DEL,p->udp_fd,NULL); close(p->udp_fd); }
    nb_ring_dispose(&p->down_tx);
    nb_ring_dispose(&p->up_tx);
    free(p->queue_clocks);p->queue_clocks=NULL;
    nb_lstream_tx_dispose(&p->logical_tx);
    free(p->logical_decoder);p->logical_decoder=NULL;
    free(p->fec_ctrl_rxbuf);p->fec_ctrl_rxbuf=NULL;
    instance_queue_release(p->fec_dg_tx_len+p->fec_stage_tx_len+p->udp_down_tx_len+
        p->udp_up_tx_len+p->udp_pending_tx_len);
    free(p->fec_dg_tx); p->fec_dg_tx=NULL;
    free(p->fec_stage_tx); p->fec_stage_tx=NULL;
    free(p->udp_down_tx);p->udp_down_tx=NULL;
    free(p->udp_up_tx);p->udp_up_tx=NULL;
    free(p->udp_pending_tx);p->udp_pending_tx=NULL;
    if(p->udp_reassembly){nb_udp_reassembly_dispose(p->udp_reassembly);free(p->udp_reassembly);p->udp_reassembly=NULL;}
    nb_udp_fec_tx_destroy(p->udp_fec_tx);p->udp_fec_tx=NULL;
    nb_udp_fec_rx_destroy(p->udp_fec_rx);p->udp_fec_rx=NULL;
    fec_session_index_remove(p->fec_session_id,p);
    if(p->fec_engine!=NULL) fec_metrics_add(&g_fec_metrics_done,nb_fec_metrics(p->fec_engine));
    nb_fec_session_destroy(p->fec_engine); p->fec_engine=NULL;
    nb_ring_dispose(&p->q2t);
    nb_session_index_remove_value(&g_session_index,p);
    if(g_ps_inuse>0) g_ps_inuse--;
    p->in_use=0;
}
/* 主动拆流: discard 两侧 QUIC stream(=reset+stop_sending), 连锁通知上/下游一并回收, 再释放本地。
 * 用于"本地 TCP 端已关闭 / 空闲超时"等一侧先结束的场景, 不再死等对端 FIN。 */
static void ps_teardown(proxy_stream_t* p){
    if(!p->in_use) return;
    if(p->down_cnx && p->down_opened) picoquic_set_app_stream_ctx(p->down_cnx, p->down_stream_id, NULL);
    if(p->up_cnx) picoquic_set_app_stream_ctx(p->up_cnx, p->up_stream_id, NULL);
    if(p->fec_ctrl_cnx && p->fec_ctrl_opened) picoquic_set_app_stream_ctx(p->fec_ctrl_cnx, p->fec_ctrl_stream_id, NULL);
    if(p->down_cnx && p->down_opened) picoquic_discard_stream(p->down_cnx, p->down_stream_id, 0);
    if(p->up_cnx) picoquic_discard_stream(p->up_cnx, p->up_stream_id, 0);
    ps_free(p);
}

static void ps_teardown_reason(proxy_stream_t* p, const char* reason){
    ps_set_close_reason(p, reason);
    ps_teardown(p);
}

/* q2t 动态追加(永不丢数据; 背压靠 QUIC connection flow control 限总量) */
static int q2t_append(proxy_stream_t* p, const uint8_t* d, size_t n){
    if(nb_send_append(&p->q2t,&p->queue_clocks->q2t,d,n,NB_Q2T_MAX,picoquic_current_time(),&p->media_q2t_peak)!=0){
        log4c_warn("id=%u q2t over limit %zu+%zu > %d",p->id,p->q2t.len,n,NB_Q2T_MAX);return -1;
    }
    if(n>0 && p->first_q2t_queue_at==0) p->first_q2t_queue_at=picoquic_current_time();
    return 0;
}

static int append_bytes(uint8_t** buf, size_t* len, size_t* cap, size_t limit, const uint8_t* d, size_t n, uint32_t ps_id, const char* tag){
    if(n == 0) return 0;
    if(*len + n > limit){
        log4c_warn("id=%u %s over limit %zu+%zu > %zu", ps_id, tag, *len, n, limit);
        return -1;
    }
    if(nb_current_instance->queue_bytes>nb_current_instance->queue_limit_bytes||
        n>nb_current_instance->queue_limit_bytes-nb_current_instance->queue_bytes){
        log4c_warn("id=%u %s instance queue budget exceeded",ps_id,tag);return -1;
    }
    if(*len + n > *cap){
        size_t ncap = *cap ? (*cap * 2) : 4096;
        while(ncap < *len + n) ncap *= 2;
        uint8_t* nb = realloc(*buf, ncap);
        if(nb == NULL){
            log4c_error("id=%u %s realloc %zu fail", ps_id, tag, ncap);
            return -1;
        }
        *buf = nb; *cap = ncap;
    }
    nb_current_instance->queue_bytes+=(uint64_t)n;memcpy(*buf + *len, d, n);
    *len += n;
    return 0;
}

static void nb_put_u16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)(v&0xFF); }
static void nb_put_u32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)(v&0xFF); }
static void nb_put_u64(uint8_t* p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
static uint16_t nb_get_u16(const uint8_t* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t nb_get_u32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t nb_get_u64(const uint8_t* p){uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|p[i];return v;}

#include "nb_node_udp_queue.inc"

static int fec_queue_datagram(proxy_stream_t* p, const uint8_t* d, size_t n){
    if(p->fec_dg_cnx == NULL) return -1;
    if(dgramq_append(&p->fec_dg_tx, &p->fec_dg_tx_len, &p->fec_dg_tx_cap, d, n, p->id) != 0){
        p->need_teardown = 1;
        return -1;
    }
    if(picoquic_mark_datagram_ready(p->fec_dg_cnx, 1) != 0){
        log4c_warn("id=%u mark datagram ready fail", p->id);
    }
    return 0;
}

static int fec_stage_bytes(proxy_stream_t* p, const uint8_t* d, size_t n, int fin){
    if(append_bytes(&p->fec_stage_tx, &p->fec_stage_tx_len, &p->fec_stage_tx_cap, NB_QTX_MAX, d, n, p->id, "fec-stage") != 0){
        p->need_teardown = 1;
        return -1;
    }
    if(fin) p->fec_stage_tx_fin = 1;
    return 0;
}

static int fec_pump_staged(proxy_stream_t* p){
    if(p==NULL||p->fec_engine==NULL||!p->fec_ctrl_peer_ready) return 0;
    uint8_t k=p->fec_config.k?p->fec_config.k:g_fec_cfg.k;
    uint8_t r=p->fec_config.r?p->fec_config.r:g_fec_cfg.r;
    size_t queued_block_limit=(size_t)(k+r)*(NB_FEC_SYMBOL_SIZE+30u);
    if(p->fec_dg_inflight>=NB_FEC_DG_INFLIGHT_MAX||p->fec_dg_tx_len>=queued_block_limit) return 0;
    if(p->fec_stage_tx_len>0){
        size_t writable=nb_fec_tx_writable(p->fec_engine);
        if(writable==0) return 0;
        size_t take=p->fec_stage_tx_len<writable?p->fec_stage_tx_len:writable;
        size_t block_bytes=(size_t)k*NB_FEC_SYMBOL_SIZE;
        if(take>block_bytes) take=block_bytes;
        int send_fin=p->fec_stage_tx_fin&&take==p->fec_stage_tx_len;
        int rc=nb_fec_tx_feed(p->fec_engine,p->fec_stage_tx,take,send_fin,picoquic_current_time());
        if(rc!=NB_FEC_OK){ p->need_teardown=1; return -1; }
        memmove(p->fec_stage_tx,p->fec_stage_tx+take,p->fec_stage_tx_len-take);
        p->fec_stage_tx_len-=take;
        instance_queue_release(take);
        if(send_fin) p->fec_stage_tx_fin=0;
    }
    if(p->fec_stage_tx_len==0&&p->fec_stage_tx_fin&&nb_fec_tx_finish_ready(p->fec_engine)){
        if(nb_fec_tx_feed(p->fec_engine,NULL,0,1,picoquic_current_time())!=NB_FEC_OK){
            p->need_teardown=1; return -1;
        }
        p->fec_stage_tx_fin=0;
    }
    return 0;
}

static int fec_ctrl_send(proxy_stream_t* p, const uint8_t* d, size_t n){
    if((p->fec_ctrl_cnx == p->up_cnx && p->fec_ctrl_stream_id == p->up_stream_id) || G.role == ROLE_EXIT){
        return queue_up(p, d, n, 0);
    }
    return queue_down(p, d, n, 0);
}

static int fec_engine_send_datagram(void* ctx, const uint8_t* data, size_t len){
    proxy_stream_t* p=(proxy_stream_t*)ctx;
    if(p==NULL||!p->in_use) return -1;
    if(fec_queue_datagram(p,data,len)!=0) return -1;
    p->fec_dg_sent++;
    return 0;
}

static int fec_engine_send_control(void* ctx, const uint8_t* data, size_t len){
    proxy_stream_t* p=(proxy_stream_t*)ctx;
    return (p!=NULL&&p->in_use)?fec_ctrl_send(p,data,len):-1;
}

static int fec_engine_deliver(void* ctx, const uint8_t* data, size_t len, int fin){
    proxy_stream_t* p=(proxy_stream_t*)ctx;
    if(p==NULL||!p->in_use) return -1;
    if(G.role==ROLE_EXIT){
        if(q2t_append(p,data,len)!=0){ p->need_teardown=1; return -1; }
        if(fin) p->q2t_fin=1;
    }else if(p->fec_rx_to_down){
        fwd_down(p,(uint8_t*)data,len,fin);
    }else{
        fwd_up(p,(uint8_t*)data,len,fin);
    }
    return p->need_teardown?-1:0;
}

static int fec_engine_ensure(proxy_stream_t* p){
    if(p->fec_engine!=NULL) return 0;
    if(fec_session_index_add(p->fec_session_id,p)!=0){
        log4c_error("id=%u duplicate fec session sid=%u",p->id,p->fec_session_id);return -1;
    }
    nb_fec_callbacks_t cb={fec_engine_send_datagram,fec_engine_send_control,fec_engine_deliver};
    const nb_fec_config_t* cfg=p->fec_config.k?&p->fec_config:&g_fec_cfg;
    p->fec_engine=nb_fec_session_create(p->fec_session_id,cfg,&cb,p);
    if(p->fec_engine==NULL){
        log4c_error("id=%u fec engine create fail sid=%u",p->id,p->fec_session_id);
        fec_session_index_remove(p->fec_session_id,p);return -1;
    }
    return 0;
}

static int fec_build_datagram(uint8_t* out, size_t cap, uint8_t type, uint32_t session_id, uint32_t seq){
    if(cap < 14) return -1;
    nb_put_u32(out, NB_FEC_DGRAM_MAGIC);
    out[4] = NB_FEC_DGRAM_VER;
    out[5] = type;
    nb_put_u32(out + 6, session_id);
    nb_put_u32(out + 10, seq);
    return 14;
}

static int fec_queue_probe_datagram(proxy_stream_t* p, uint8_t type){
    uint8_t buf[32];
    int n = fec_build_datagram(buf, sizeof(buf), type, p->fec_session_id, (uint32_t)(++p->fec_dg_sent));
    return (n > 0) ? fec_queue_datagram(p, buf, (size_t)n) : -1;
}

static int fec_should_drop(const proxy_stream_t* p,uint8_t type, uint32_t block_id, uint8_t symbol_idx){
    (void)block_id;
    if(type == nb_fec_dgram_source && g_fec_v15_drop_src_mod > 0){
        uint8_t k=p&&p->fec_config.k?p->fec_config.k:g_fec_cfg.k;
        uint64_t seq = (uint64_t)block_id * k + symbol_idx + 1;
        if((seq % g_fec_v15_drop_src_mod) == 0){ g_fec_v15_drop_src++; return 1; }
    } else if(type == nb_fec_dgram_repair && g_fec_v15_drop_repair_mod > 0){
        uint64_t seq = (uint64_t)block_id + symbol_idx + 1;
        if((seq % g_fec_v15_drop_repair_mod) == 0){ g_fec_v15_drop_repair++; return 1; }
    }
    return 0;
}

static void fec_ctrl_handle_line(proxy_stream_t* p, const char* line, int from_down){
    uint32_t sid=0;char route[300];
    if(strncmp(line, NB_FEC_CTRL_START ":", strlen(NB_FEC_CTRL_START ":")) == 0){
        nb_fec_start_t start;
        if(nb_fec_start_parse(line,g_fec_cfg.k,g_fec_cfg.r,&start)==0){
            sid=start.session_id;int prio=start.priority;
            char tenant[NB_TENANT_NAME_MAX];const char* routed=start.route;
            int tenant_wire=nb_tenant_wire_parse(start.route,tenant,sizeof(tenant),&routed);
            if(tenant_wire<0){p->need_teardown=1;return;}
            if(tenant_wire>0)snprintf(p->tenant_name,sizeof(p->tenant_name),"%s",tenant);
            if(G.role==ROLE_EXIT&&g_tenants_enabled){
                p->tenant_index=tenant_wire>0?nb_tenant_find(&g_tenants,tenant):(g_tenants.count==1?0:-1);
                if(p->tenant_index<0){ps_set_close_reason(p,"tenant-fec-unknown");p->need_teardown=1;return;}
                snprintf(p->tenant_name,sizeof(p->tenant_name),"%s",g_tenants.items[p->tenant_index].name);
            }
            snprintf(route,sizeof(route),"%s",routed);
            p->fec_ctrl_only = 1;
            p->fec_sidecar_mode = 1;
            p->fec_session_id = sid;
            p->prio = prio;
            if(route[0]) snprintf(p->fec_route, sizeof(p->fec_route), "%s", route);
            p->fec_dg_cnx = p->up_cnx;
            p->fec_ctrl_cnx = p->up_cnx;
            p->fec_ctrl_stream_id = p->up_stream_id;
            p->fec_ctrl_opened = 1;
            if(nb_fec_config_for_bdp(&p->fec_config,start.k,start.r,g_fec_bdp_bitrate_bps,
                g_fec_bdp_rtt_us,g_fec_bdp_multiplier)!=NB_FEC_OK){p->need_teardown=1;return;}
            if(fec_engine_ensure(p)!=0){ p->need_teardown=1; return; }
            log4c_info("%s fec ctrl START sid=%u prio=%d k=%u r=%u route=%s", role_name(G.role), sid, prio,
                start.k,start.r,p->fec_route);
            if(G.role == ROLE_MIDDLE && !from_down && route[0]){
                char first[300], rest[300]; rest[0]=0;
                char* comma=strchr(route, ',');
                if(comma){
                    size_t fl=(size_t)(comma-route); if(fl>=sizeof(first)) fl=sizeof(first)-1;
                    memcpy(first, route, fl); first[fl]=0; snprintf(rest, sizeof(rest), "%s", comma+1);
                } else {
                    snprintf(first, sizeof(first), "%s", route);
                }
                if(strncmp(first,"H:",2)==0){
                    char nhost[256]; int nport=0; char* c=strrchr(first+2,':');
                    if(c){
                        size_t host_len=(size_t)(c-(first+2));
                        if(host_len==0||host_len>=sizeof(nhost)||parse_port_strict(c+1,&nport)!=0){ps_teardown(p);return;}
                        memcpy(nhost,first+2,host_len);nhost[host_len]=0;
                        struct sockaddr_storage naddr; int is_name=0;
                        nb_flow_policy_t pol;
                        char thost[256]; int tport=0;
                        if(parse_target_from_route(rest[0]?rest:"", thost, sizeof(thost), &tport) == 0) flow_policy_for_host(thost, tport, &pol);
                        else nb_flow_policy_default(&pol);
                        if(!pol.matched) pol.prio = prio;
                        if(picoquic_get_server_address(nhost,nport,&naddr,&is_name)==0){
                            p->down_pool_id=pool_get_or_create(&naddr);
                            if(p->down_pool_id<0){ps_teardown(p);return;}
                            if(downstream_open_stream(p, rest[0]?rest:"", &pol)!=0){ ps_teardown(p); return; }
                            p->fec_rx_to_down = 1;
                        }
                    }
                }
            }
            if(G.role == ROLE_EXIT && p->tcp_fd < 0 && route[0]){
                char thost[256]; int tport=0;
                if(fec_parse_target_route(route, thost, sizeof(thost), &tport) == 0){
                    if(!whitelist_allowed(thost,tport)){
                        log4c_debug("exit id=%u fec ctrl BLOCKED %s:%d (not in whitelist)",p->id,thost,tport);
                        ps_teardown(p);
                        return;
                    }
                    if(dns_submit(p->id,thost,tport)!=0){
                        log4c_error("id=%u fec ctrl dns queue full %s:%d",p->id,thost,tport);
                        ps_teardown(p);
                        return;
                    }
                    p->dns_pending=1; p->target_port=tport;
                }
            }
            if(G.role == ROLE_EXIT && !from_down){
                char ack[64];
                int al = snprintf(ack, sizeof(ack), NB_FEC_CTRL_ACK ":%u\n", sid);
                if(queue_up(p, (const uint8_t*)ack, (size_t)al, 0) != 0){
                    log4c_warn("exit fec ctrl ack queue fail sid=%u", sid);
                }
            }
        }
    } else if(sscanf(line, "FC:ACK:%u", &sid) == 1){
        proxy_stream_t* s = from_down ? p : ps_find_by_fec_session(sid);
        if(s != NULL){
            s->fec_ctrl_peer_ready = 1;
            s->fec_sidecar_mode = 1;
            log4c_info("%s fec ctrl ACK sid=%u", role_name(G.role), sid);
            if(G.role == ROLE_MIDDLE && from_down){
                int fc_ret=picoquic_open_flow_control(s->fec_ctrl_cnx,s->fec_ctrl_stream_id,4u*1024u*1024u);
                if(fc_ret!=0){
                    log4c_warn("middle fec ctrl flow credit failed sid=%u stream=%llu ret=%d",
                        sid,(unsigned long long)s->fec_ctrl_stream_id,fc_ret);
                    s->need_teardown=1;
                    return;
                }
                if(!s->need_teardown) (void)fec_pump_staged(s);
                (void)fec_queue_probe_datagram(s, nb_fec_dgram_hello);
            }
        }
    } else if(p->fec_engine!=NULL){
        int rc=nb_fec_on_control(p->fec_engine,line,strlen(line),picoquic_current_time());
        if(rc!=NB_FEC_OK){
            log4c_warn("%s fec engine control fail sid=%u rc=%d line=%.32s",role_name(G.role),p->fec_session_id,rc,line);
            p->need_teardown=1;
        }
        if(rc==NB_FEC_OK) (void)fec_pump_staged(p);
        return;
    } else {
        log4c_warn("%s fec control without engine: %.32s",role_name(G.role),line);
        p->need_teardown=1;
    }
}

static void fec_ctrl_feed_bytes(proxy_stream_t* p, uint8_t* data, size_t n, int from_down){
    if(n == 0) return;
    if(p->fec_ctrl_rxbuf==NULL){
        p->fec_ctrl_rxbuf=calloc(1,NB_FEC_CTRL_RX_CAP);
        if(p->fec_ctrl_rxbuf==NULL){p->need_teardown=1;return;}
    }
    if(p->fec_ctrl_rxlen>=NB_FEC_CTRL_RX_CAP-1){p->need_teardown=1;return;}
    size_t cp = n;
    if(p->fec_ctrl_rxlen + cp >= NB_FEC_CTRL_RX_CAP) cp = NB_FEC_CTRL_RX_CAP - p->fec_ctrl_rxlen - 1;
    memcpy(p->fec_ctrl_rxbuf + p->fec_ctrl_rxlen, data, cp);
    p->fec_ctrl_rxlen += cp;
    p->fec_ctrl_rxbuf[p->fec_ctrl_rxlen] = 0;
    char* nl;
    while((nl = strchr(p->fec_ctrl_rxbuf, '\n')) != NULL){
        *nl = 0;
        fec_ctrl_handle_line(p, p->fec_ctrl_rxbuf, from_down);
        size_t used = (size_t)(nl - p->fec_ctrl_rxbuf + 1);
        memmove(p->fec_ctrl_rxbuf, p->fec_ctrl_rxbuf + used, p->fec_ctrl_rxlen - used);
        p->fec_ctrl_rxlen -= used;
        p->fec_ctrl_rxbuf[p->fec_ctrl_rxlen] = 0;
    }
}

static void middle_upstream_fc_set(proxy_stream_t* p,int enabled){
    if(G.role!=ROLE_MIDDLE||p==NULL||p->up_cnx==NULL||p->udp_mode)return;
    enabled=enabled!=0;
    if(p->upstream_fc_enabled==enabled)return;
    int ret=picoquic_set_app_flow_control(p->up_cnx,p->up_stream_id,enabled);
    if(ret!=0){
        log4c_warn("middle upstream fc mode failed id=%u sid=%llu enabled=%d ret=%d",
            p->id,(unsigned long long)p->up_stream_id,enabled,ret);
        return;
    }
    p->upstream_fc_enabled=enabled;p->upstream_fc_blocked=0;
    if(enabled)middle_upstream_fc_update(p,picoquic_current_time());
    else{
        /* A provisional 128 KiB window was used until the route was known.
         * Non-media streams return to picoquic automatic flow control. */
        (void)picoquic_open_flow_control(p->up_cnx,p->up_stream_id,1024*1024ULL);
    }
}

static void middle_upstream_fc_update(proxy_stream_t* p,uint64_t now_us){
    if(G.role!=ROLE_MIDDLE||p==NULL||!p->upstream_fc_enabled||p->up_cnx==NULL)return;
    nb_live_queue_limits_t limits=nb_live_queue_limits(p->flow_class,p->udp_mode);
    size_t queued=p->down_tx.len;
    uint64_t age=nb_live_queue_age_us(&p->queue_clocks->down_tx,queued,now_us);
    int should_block=queued>=limits.high_bytes||age>=limits.deadline_us;
    if(should_block&&!p->upstream_fc_blocked){
        p->upstream_fc_blocked=1;
        log4c_info("middle upstream fc pause id=%u class=%s queue=%zu age=%.1fms high=%zu target=%.1fms route=%s",
            p->id,nb_flow_class_name(p->flow_class),queued,age/1000.0,limits.high_bytes,
            limits.deadline_us/1000.0,p->route);
    }
    if(p->upstream_fc_blocked){
        if(queued>limits.low_bytes||age>=limits.deadline_us)return;
        p->upstream_fc_blocked=0;
        log4c_info("middle upstream fc resume id=%u class=%s queue=%zu age=%.1fms low=%zu route=%s",
            p->id,nb_flow_class_name(p->flow_class),queued,age/1000.0,limits.low_bytes,p->route);
    }
    if(queued<limits.high_bytes){
        uint64_t available=(uint64_t)(limits.high_bytes-queued);
        if(available>NB_STREAM_FLOW_CREDIT_QUANTUM)available=NB_STREAM_FLOW_CREDIT_QUANTUM;
        int ret=picoquic_open_flow_control(p->up_cnx,p->up_stream_id,available);
        if(ret!=0)log4c_warn("middle upstream fc grant failed id=%u sid=%llu credit=%llu ret=%d",
            p->id,(unsigned long long)p->up_stream_id,(unsigned long long)available,ret);
    }
}

static int queue_down(proxy_stream_t* p, const uint8_t* d, size_t n, int fin){
    uint64_t now=picoquic_current_time();
    if(nb_send_append(&p->down_tx,&p->queue_clocks->down_tx,d,n,NB_QTX_MAX,now,&p->media_down_q_peak)!=0){
        p->need_teardown = 1;
        return -1;
    }
    middle_upstream_fc_update(p,now);
    if(n>0 && p->first_down_queue_at==0){
        p->first_down_queue_at=picoquic_current_time();
        if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=c2s-queue age=%.1fms bytes=%zu q=%zu route=%s",
            role_name(G.role),p->id,trace_delta_ms(p->first_down_queue_at,p->created_at),n,p->down_tx.len,p->route);
    }
    if(fin) p->down_tx_fin = 1;
    if(p->down_cnx && p->down_opened){
        picoquic_mark_active_stream(p->down_cnx, p->down_stream_id, 1, p);
    }
    return 0;
}

static int queue_down_prefix(proxy_stream_t* p, const uint8_t* d, size_t n){
    if(p==NULL||d==NULL||n==0||n>sizeof(p->down_prefix)||p->down_prefix_off<p->down_prefix_len){
        if(p!=NULL)p->need_teardown=1;
        return -1;
    }
    memcpy(p->down_prefix,d,n);
    p->down_prefix_len=n;
    p->down_prefix_off=0;
    if(p->down_prefix_queued_at==0) p->down_prefix_queued_at=picoquic_current_time();
    if(p->down_cnx&&p->down_opened){
        picoquic_mark_active_stream(p->down_cnx,p->down_stream_id,1,p);
    }
    return 0;
}

static int queue_up(proxy_stream_t* p, const uint8_t* d, size_t n, int fin){
    if(nb_send_append(&p->up_tx,&p->queue_clocks->up_tx,d,n,NB_QTX_MAX,picoquic_current_time(),&p->media_up_q_peak)!=0){
        p->need_teardown = 1;
        return -1;
    }
    if(n>0 && p->first_up_queue_at==0){
        p->first_up_queue_at=picoquic_current_time();
        if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=s2c-queue age=%.1fms bytes=%zu q=%zu route=%s",
            role_name(G.role),p->id,trace_delta_ms(p->first_up_queue_at,p->created_at),n,p->up_tx.len,p->route);
    }
    if(fin) p->up_tx_fin = 1;
    if(p->up_cnx){
        picoquic_mark_active_stream(p->up_cnx, p->up_stream_id, 1, p);
    }
    return 0;
}

static int logical_queue_frame(proxy_stream_t* p,int to_down,uint8_t type,uint64_t offset,
    const uint8_t* payload,uint32_t payload_length){
    uint8_t wire[NB_LSTREAM_HEADER+NB_LSTREAM_DATA_MAX];
    int length=nb_lstream_frame_encode(wire,sizeof(wire),type,p->logical_flow_hi,p->logical_flow_lo,
        offset,payload,payload_length);
    if(length<0)return -1;
    return to_down?queue_down(p,wire,(size_t)length,0):queue_up(p,wire,(size_t)length,0);
}

static int logical_replay(proxy_stream_t* p,int to_down){
    nb_lstream_tx_t* tx=&p->logical_tx;size_t position=0;
    while(position<tx->replay_length){size_t part=tx->replay_length-position;
        if(part>NB_LSTREAM_DATA_MAX)part=NB_LSTREAM_DATA_MAX;
        if(logical_queue_frame(p,to_down,NB_LSTREAM_DATA,tx->base_offset+position,
            tx->replay+position,(uint32_t)part)!=0)return -1;
        position+=part;}
    if(tx->fin_queued&&!tx->fin_acked&&logical_queue_frame(p,to_down,NB_LSTREAM_FIN,tx->fin_offset,NULL,0)!=0)return -1;
    return 0;
}

static int logical_send_data(proxy_stream_t* p,int to_down,const uint8_t* data,size_t length,int fin){
    size_t position=0;
    while(position<length){size_t part=length-position;if(part>NB_LSTREAM_DATA_MAX)part=NB_LSTREAM_DATA_MAX;
        uint64_t offset=0;if(nb_lstream_tx_append(&p->logical_tx,data+position,part,&offset)!=0)return -1;
        if(logical_queue_frame(p,to_down,NB_LSTREAM_DATA,offset,data+position,(uint32_t)part)!=0)return -1;
        position+=part;}
    if(fin&&p->logical_tx.fin_queued)return 0;
    if(fin){if(nb_lstream_tx_fin(&p->logical_tx)!=0||logical_queue_frame(p,to_down,NB_LSTREAM_FIN,
        p->logical_tx.fin_offset,NULL,0)!=0)return -1;}
    return 0;
}

static int logical_send_ack(proxy_stream_t* p,int force){
    uint64_t now=picoquic_current_time();
    if(!force&&p->logical_rx_delivered-p->logical_last_ack_offset<16384&&
        p->logical_last_ack_at&&now-p->logical_last_ack_at<20000)return 0;
    if(p->logical_rx_delivered==p->logical_last_ack_offset&&!force)return 0;
    int to_down=G.role==ROLE_ENTRY;
    if(logical_queue_frame(p,to_down,NB_LSTREAM_ACK,p->logical_rx_delivered,NULL,0)!=0)return -1;
    p->logical_last_ack_offset=p->logical_rx_delivered;p->logical_last_ack_at=now;return 0;
}

static int logical_receive_frame(void* context,const nb_lstream_frame_t* frame){
    proxy_stream_t* p=context;if(p==NULL||!p->in_use||!p->logical_endpoint||
        frame->flow_hi!=p->logical_flow_hi||frame->flow_lo!=p->logical_flow_lo)return -1;
    if(frame->type<=NB_LSTREAM_FIN_ACK)p->logical_rx_frames[frame->type]++;
    int to_down=G.role==ROLE_ENTRY;
    if(frame->type==NB_LSTREAM_OPEN)return frame->offset==0?0:-1;
    if(frame->type==NB_LSTREAM_ACK)return nb_lstream_tx_ack(&p->logical_tx,frame->offset);
    if(frame->type==NB_LSTREAM_FIN_ACK){if(!p->logical_tx.fin_queued||frame->offset!=p->logical_tx.fin_offset)return -1;
        p->logical_tx.fin_acked=1;return nb_lstream_tx_ack(&p->logical_tx,frame->offset);}
    if(frame->type==NB_LSTREAM_DATA){
        uint64_t end=frame->offset+frame->payload_length;if(end<frame->offset)return -1;
        if(end<=p->logical_rx_next){(void)logical_send_ack(p,1);return 0;}
        if(frame->offset!=p->logical_rx_next)return -1;
        if(q2t_append(p,frame->payload,frame->payload_length)!=0)return -1;
        p->logical_rx_next=end;
        if(p->logical_rx_fin_seen&&p->logical_rx_next==p->logical_rx_fin_offset)p->q2t_fin=1;
        return 0;
    }
    if(frame->type==NB_LSTREAM_FIN){if(frame->offset<p->logical_rx_next)return -1;
        p->logical_rx_fin_seen=1;p->logical_rx_fin_offset=frame->offset;
        if(frame->offset==p->logical_rx_next)p->q2t_fin=1;
        if(G.role==ROLE_ENTRY)p->down_fin_seen=1;else p->up_fin_seen=1;return 0;}
    if(frame->type==NB_LSTREAM_RESUME||frame->type==NB_LSTREAM_RESUME_ACK){
        if(nb_lstream_tx_ack(&p->logical_tx,frame->offset)!=0)return -1;
        if(frame->type==NB_LSTREAM_RESUME&&logical_queue_frame(p,to_down,NB_LSTREAM_RESUME_ACK,
            p->logical_rx_delivered,NULL,0)!=0)return -1;
        int result=logical_replay(p,to_down);
        if(result==0&&frame->type==NB_LSTREAM_RESUME_ACK&&G.role==ROLE_ENTRY){
            p->logical_resume_inflight=0;p->logical_reconnect_failures=0;
            p->logical_disconnected_at=0;p->logical_reconnect_last_attempt=0;
            log4c_info("entry logical flow resume confirmed id=%u",p->id);
        }
        return result;
    }
    if(frame->type==NB_LSTREAM_RST){p->need_teardown=1;return 0;}
    return -1;
}

static int logical_receive(proxy_stream_t* p,const uint8_t* data,size_t length){
    if(p==NULL||p->logical_decoder==NULL)return -1;
    return nb_lstream_decoder_feed(p->logical_decoder,data,length,logical_receive_frame,p);
}

static void logical_note_delivered(proxy_stream_t* p,size_t length){
    if(p==NULL||!p->logical_endpoint||length==0)return;
    p->logical_rx_delivered+=length;
    if(logical_send_ack(p,p->logical_rx_fin_seen&&p->logical_rx_delivered==p->logical_rx_fin_offset)!=0)p->need_teardown=1;
}

static void logical_note_fin_delivered(proxy_stream_t* p){
    if(p==NULL||!p->logical_endpoint||!p->logical_rx_fin_seen||p->logical_rx_fin_acked_sent||
        p->logical_rx_delivered!=p->logical_rx_fin_offset)return;
    int to_down=G.role==ROLE_ENTRY;
    if(logical_queue_frame(p,to_down,NB_LSTREAM_FIN_ACK,p->logical_rx_fin_offset,NULL,0)!=0)p->need_teardown=1;
    else p->logical_rx_fin_acked_sent=1;
}

static void logical_detach(proxy_stream_t* p,picoquic_cnx_t* cnx){
    if(p==NULL||!p->logical_endpoint)return;
    nb_session_index_remove_value(&g_session_index,p);
    if(G.role==ROLE_ENTRY&&p->down_cnx==cnx){p->down_cnx=NULL;p->down_opened=0;p->down_prefix_len=p->down_prefix_off=0;
        if(p->logical_resume_inflight&&p->logical_reconnect_failures<UINT_MAX)p->logical_reconnect_failures++;
        p->logical_resume_inflight=0;nb_send_clear(&p->down_tx,&p->queue_clocks->down_tx);p->logical_reconnect_pending=1;}
    else if(G.role==ROLE_EXIT&&p->up_cnx==cnx){p->up_cnx=NULL;nb_send_clear(&p->up_tx,&p->queue_clocks->up_tx);p->logical_reconnect_pending=1;}
    nb_send_clear(&p->q2t,&p->queue_clocks->q2t);p->logical_rx_next=p->logical_rx_delivered;
    if(!p->logical_rx_fin_acked_sent){p->logical_rx_fin_seen=0;p->logical_rx_fin_offset=0;p->q2t_fin=0;}
    if(p->logical_decoder)nb_lstream_decoder_init(p->logical_decoder);
    if(p->logical_disconnected_at==0)p->logical_disconnected_at=picoquic_current_time();
    ps_index_refresh(p);
}

/* 非阻塞 connect 目标 TCP(exit): 用已(异步)解析好的地址, 不在事件循环内做阻塞 DNS */
static int tcp_connect_addr(struct sockaddr* sa, socklen_t sl){
    int fd=socket(sa->sa_family,SOCK_STREAM,0);
    if(fd<0) return -1;
    set_nonblock(fd); int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    if(G.outbound_configured){
        if(G.outbound_addr.ss_family!=sa->sa_family){close(fd);errno=EAFNOSUPPORT;return -1;}
        socklen_t source_len=sa->sa_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
        if(bind(fd,(struct sockaddr*)&G.outbound_addr,source_len)!=0){close(fd);return -1;}
    }
    if(connect(fd,sa,sl)<0 && errno!=EINPROGRESS){ close(fd); return -1; }
    return fd;
}

static int udp_connect_addr(struct sockaddr* sa,socklen_t sl){
    int fd=socket(sa->sa_family,SOCK_DGRAM,0);if(fd<0)return -1;
    if(nb_udp_enable_rxq_overflow(fd)!=0){int saved=errno;close(fd);errno=saved;return -1;}
    if(set_nonblock(fd)!=0){close(fd);return -1;}
    if(G.outbound_configured){
        if(G.outbound_addr.ss_family!=sa->sa_family){close(fd);errno=EAFNOSUPPORT;return -1;}
        socklen_t source_len=sa->sa_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
        if(bind(fd,(struct sockaddr*)&G.outbound_addr,source_len)!=0){close(fd);return -1;}
    }
    if(connect(fd,sa,sl)!=0){close(fd);return -1;}return fd;
}

static int relay_quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t ev, void* cb_ctx, void* v_stream_ctx);
static int epoll_udp_update(proxy_stream_t* p,int add);
static void udp_local_drain(proxy_stream_t* p);

/* 确保池中 idx 槽有一条已启动的连接(空槽则新建 + start + 保活)。返回 0=可用 -1=失败 */
