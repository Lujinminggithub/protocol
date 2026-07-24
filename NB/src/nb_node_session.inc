static proxy_stream_t* ps_alloc(void){
    int i; for(i=0;i<MAX_CONN;i++) if(!G.streams[i].in_use){
        memset(&G.streams[i],0,sizeof(G.streams[i]));
        G.streams[i].in_use=1; G.streams[i].tcp_fd=-1; G.streams[i].udp_fd=-1; G.streams[i].id=++G.next_ps_id;
        G.streams[i].down_pool_idx=-1;
        G.streams[i].down_pool_id=-1;
        G.streams[i].tenant_index=-1;
        G.streams[i].route_index=-1;
        G.streams[i].last_active=picoquic_current_time();
        G.streams[i].created_at=G.streams[i].last_active;
        nb_udp_reassembly_init(&G.streams[i].udp_reassembly);
        (void)nb_session_index_put(&g_session_index,NB_SESSION_KEY_ID,G.streams[i].id,0,0,&G.streams[i]);
        g_ps_inuse++; if(g_ps_inuse>g_ps_peak) g_ps_peak=g_ps_inuse;
        return &G.streams[i]; }
    return NULL;
}
static void ps_touch(proxy_stream_t* p){ p->last_active=picoquic_current_time(); }
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
    if(p->close_reason[0] == 0) ps_set_close_reason(p, "released");
    if(p->udp_mode&&!p->udp_association&&!p->udp_close_received){if(G.role!=ROLE_EXIT)(void)udp_lifecycle_schedule(p->down_cnx,NB_UDP_TYPE_CLOSE,p->udp_session_id,p->route);if(G.role!=ROLE_ENTRY)(void)udp_lifecycle_schedule(p->up_cnx,NB_UDP_TYPE_CLOSE,p->udp_session_id,p->route);}
    if(g_tenants_enabled&&p->tenant_index>=0){nb_tenant_account(&g_tenants,p->tenant_index,p->bytes_c2s-p->tenant_accounted_up,p->bytes_s2c-p->tenant_accounted_down);if(p->tenant_acquired)nb_tenant_release(&g_tenants,p->tenant_index,p->tenant_udp);}
    /* Business-session lifetime is not a route-health signal. Active probes own health updates. */
    if(G.role==ROLE_ENTRY&&p->route_index>=0)nb_routes_finish_session(&G.exit_routes,(size_t)p->route_index);
    nb_metrics_note_close(&g_metrics_state,p->close_reason);
    if(p->udp_mode && !p->udp_association){
        log4c_info("%s udp flow close id=%u usid=%u reason=%s age=%.1fms packets=%llu/%llu bytes=%llu/%llu milestones_ms=local_c2s:%.1f,c2s_rx:%.1f,c2s_prepare:%.1f,target_ready:%.1f,target_send:%.1f,target_rx:%.1f,s2c_rx:%.1f,s2c_prepare:%.1f,local_s2c:%.1f target=%s:%d route=%s",
            role_name(G.role),p->id,p->udp_session_id,p->close_reason,
            (picoquic_current_time()-p->created_at)/1000.0,
            (unsigned long long)p->udp_packets_c2s,(unsigned long long)p->udp_packets_s2c,
            (unsigned long long)p->bytes_c2s,(unsigned long long)p->bytes_s2c,
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
    free(p->fec_dg_tx); p->fec_dg_tx=NULL;
    free(p->fec_stage_tx); p->fec_stage_tx=NULL;
    free(p->udp_down_tx);p->udp_down_tx=NULL;
    free(p->udp_up_tx);p->udp_up_tx=NULL;
    free(p->udp_pending_tx);p->udp_pending_tx=NULL;
    nb_udp_reassembly_dispose(&p->udp_reassembly);
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
    if(nb_send_append(&p->q2t,&p->q2t_clock,d,n,NB_Q2T_MAX,picoquic_current_time(),&p->media_q2t_peak)!=0){
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
    memcpy(*buf + *len, d, n);
    *len += n;
    return 0;
}

static void nb_put_u16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)(v&0xFF); }
static void nb_put_u32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)(v&0xFF); }
static void nb_put_u64(uint8_t* p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
static uint16_t nb_get_u16(const uint8_t* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t nb_get_u32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t nb_get_u64(const uint8_t* p){uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|p[i];return v;}

static int dgramq_append(uint8_t** buf, size_t* len, size_t* cap, const uint8_t* d, size_t n, uint32_t ps_id){
    size_t need = 10 + n;
    if(*len + need > NB_DGRAM_QUEUE_MAX){
        log4c_warn("id=%u fec datagram queue over limit %zu+%zu > %d", ps_id, *len, need, NB_DGRAM_QUEUE_MAX);
        return -1;
    }
    if(*len + need > *cap){
        size_t ncap = *cap ? (*cap * 2) : 4096;
        while(ncap < *len + need) ncap *= 2;
        uint8_t* nb = realloc(*buf, ncap);
        if(nb == NULL){
            log4c_error("id=%u fec datagram queue realloc %zu fail", ps_id, ncap);
            return -1;
        }
        *buf = nb; *cap = ncap;
    }
    nb_put_u16(*buf + *len, (uint16_t)n);
    nb_put_u64(*buf + *len + 2,picoquic_current_time());
    memcpy(*buf + *len + 10, d, n);
    *len += need;
    return 0;
}

static int dgramq_peek(uint8_t* buf, size_t len, const uint8_t** d, size_t* n,uint64_t* queued_at){
    if(len < 10) return 0;
    *n = nb_get_u16(buf);
    if(len < 10 + *n) return 0;
    if(queued_at)*queued_at=nb_get_u64(buf+2);
    *d = buf + 10;
    return 1;
}

static void dgramq_consume(uint8_t* buf, size_t* len){
    const uint8_t* d=NULL; size_t n=0;
    if(!dgramq_peek(buf,*len,&d,&n,NULL)) return;
    memmove(buf, buf + 10 + n, *len - (10 + n));
    *len -= (10 + n);
}

static uint64_t dgramq_drop_expired(uint8_t* buf,size_t* len,uint64_t deadline_us,uint64_t now){
    uint64_t dropped=0,queued_at=0;const uint8_t* data=NULL;size_t data_len=0;
    while(dgramq_peek(buf,*len,&data,&data_len,&queued_at)&&queued_at>0&&now>queued_at&&
        now-queued_at>deadline_us){dgramq_consume(buf,len);dropped++;}
    return dropped;
}

static int udp_queue_wire(proxy_stream_t* p,int to_down,const uint8_t* data,size_t length){
    uint8_t** buf=to_down?&p->udp_down_tx:&p->udp_up_tx;
    size_t* len=to_down?&p->udp_down_tx_len:&p->udp_up_tx_len;
    size_t* cap=to_down?&p->udp_down_tx_cap:&p->udp_up_tx_cap;
    picoquic_cnx_t* cnx=to_down?p->down_cnx:p->up_cnx;
    if(cnx==NULL||dgramq_append(buf,len,cap,data,length,p->id)!=0)return -1;
    if(to_down&&*len>p->media_down_q_peak)p->media_down_q_peak=*len;
    if(!to_down&&*len>p->media_up_q_peak)p->media_up_q_peak=*len;
    if(picoquic_mark_datagram_ready(cnx,1)!=0)return -1;
    return 0;
}

static int udp_queue_packet(proxy_stream_t* p,int to_down,uint8_t type,const char* route,
    const uint8_t* payload,size_t payload_length){
    uint16_t fragments=nb_udp_fragment_count(payload_length);
    size_t route_length=route?strlen(route):0;
    if(fragments==0||route_length==0||route_length>=NB_UDP_ROUTE_MAX)return -1;
    uint32_t sequence=++p->udp_tx_sequence;
    for(uint16_t i=0;i<fragments;i++){
        size_t offset=(size_t)i*NB_UDP_FRAGMENT_PAYLOAD;
        size_t part=payload_length-offset;if(part>NB_UDP_FRAGMENT_PAYLOAD)part=NB_UDP_FRAGMENT_PAYLOAD;
        uint8_t wire[NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32];
        int n=nb_udp_wire_encode(wire,sizeof(wire),type,p->udp_session_id,sequence,i,fragments,
            (uint16_t)payload_length,route,(uint16_t)route_length,payload+offset,(uint16_t)part);
        if(n<0||udp_queue_wire(p,to_down,wire,(size_t)n)!=0)return -1;
    }
    return 0;
}

static int udp_forward_fragment(proxy_stream_t* p,int to_down,const nb_udp_wire_view_t* view,
    const char* route){
    size_t route_length=strlen(route);uint8_t wire[NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32];
    int n=nb_udp_wire_encode(wire,sizeof(wire),view->type,view->session_id,view->sequence,
        view->fragment_index,view->fragment_count,view->total_length,route,(uint16_t)route_length,
        view->payload,view->payload_length);
    return n<0?-1:udp_queue_wire(p,to_down,wire,(size_t)n);
}

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
    p->fec_engine=nb_fec_session_create(p->fec_session_id,&g_fec_cfg,&cb,p);
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

static int fec_should_drop(uint8_t type, uint32_t block_id, uint8_t symbol_idx){
    (void)block_id;
    if(type == nb_fec_dgram_source && g_fec_v15_drop_src_mod > 0){
        uint64_t seq = (uint64_t)block_id * NB_FEC_V15_K + symbol_idx + 1;
        if((seq % g_fec_v15_drop_src_mod) == 0){ g_fec_v15_drop_src++; return 1; }
    } else if(type == nb_fec_dgram_repair && g_fec_v15_drop_repair_mod > 0){
        uint64_t seq = (uint64_t)block_id + symbol_idx + 1;
        if((seq % g_fec_v15_drop_repair_mod) == 0){ g_fec_v15_drop_repair++; return 1; }
    }
    return 0;
}

static void fec_ctrl_handle_line(proxy_stream_t* p, const char* line, int from_down){
    uint32_t sid=0; int prio=0; char route[300];
    if(strncmp(line, NB_FEC_CTRL_START ":", strlen(NB_FEC_CTRL_START ":")) == 0){
        route[0]=0;
        if(sscanf(line, "FC:START:%u:%d:%299[^\n]", &sid, &prio, route) >= 2){
            p->fec_ctrl_only = 1;
            p->fec_sidecar_mode = 1;
            p->fec_session_id = sid;
            p->prio = prio;
            if(route[0]) snprintf(p->fec_route, sizeof(p->fec_route), "%s", route);
            p->fec_dg_cnx = p->up_cnx;
            p->fec_ctrl_cnx = p->up_cnx;
            p->fec_ctrl_stream_id = p->up_stream_id;
            p->fec_ctrl_opened = 1;
            if(fec_engine_ensure(p)!=0){ p->need_teardown=1; return; }
            log4c_info("%s fec ctrl START sid=%u prio=%d route=%s", role_name(G.role), sid, prio, p->fec_route);
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
                if((s->fec_stage_tx_len > 0 || s->fec_stage_tx_fin) && !s->need_teardown){
                    if(nb_fec_tx_feed(s->fec_engine, s->fec_stage_tx, s->fec_stage_tx_len,
                        s->fec_stage_tx_fin, picoquic_current_time()) != 0){
                        s->need_teardown = 1;
                    }
                    s->fec_stage_tx_len = 0;
                    s->fec_stage_tx_fin = 0;
                }
                (void)fec_queue_probe_datagram(s, nb_fec_dgram_hello);
            }
        }
    } else if(p->fec_engine!=NULL){
        int rc=nb_fec_on_control(p->fec_engine,line,strlen(line),picoquic_current_time());
        if(rc!=NB_FEC_OK){
            log4c_warn("%s fec engine control fail sid=%u rc=%d line=%.32s",role_name(G.role),p->fec_session_id,rc,line);
            p->need_teardown=1;
        }
        return;
    } else {
        log4c_warn("%s fec control without engine: %.32s",role_name(G.role),line);
        p->need_teardown=1;
    }
}

static void fec_ctrl_feed_bytes(proxy_stream_t* p, uint8_t* data, size_t n, int from_down){
    if(n == 0) return;
    size_t cp = n;
    if(p->fec_ctrl_rxlen + cp >= sizeof(p->fec_ctrl_rxbuf)) cp = sizeof(p->fec_ctrl_rxbuf) - p->fec_ctrl_rxlen - 1;
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
    uint64_t age=nb_live_queue_age_us(&p->down_tx_clock,queued,now_us);
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
    if(nb_send_append(&p->down_tx,&p->down_tx_clock,d,n,NB_QTX_MAX,now,&p->media_down_q_peak)!=0){
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
    if(nb_send_append(&p->up_tx,&p->up_tx_clock,d,n,NB_QTX_MAX,picoquic_current_time(),&p->media_up_q_peak)!=0){
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
