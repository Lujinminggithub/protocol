static int entry_open_upstream(void){
    return pool_get_or_create(&G.next_addr)<0?-1:0;
}

static int socks_udp_bind_socket(struct sockaddr_in* bound){
    return nb_udp_bind_relay_socket(g_socks_udp_port_min,g_socks_udp_port_max,
        &g_socks_udp_port_next,bound);
}

static int entry_accept_one(void){
    struct sockaddr_storage peer;
    socklen_t peer_len=sizeof(peer);
    int fd=accept(G.tcp_listen_fd,(struct sockaddr*)&peer,&peer_len);
    if(fd<0) return 0;
    set_nonblock(fd); int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    proxy_stream_t* p=ps_alloc();
    if(!p){ log4c_warn("stream table full, drop tcp accept"); close(fd); return 1; }
    p->tcp_fd=fd;
    sockaddr_to_text((struct sockaddr*)&peer,peer_len,p->peer_addr,sizeof(p->peer_addr));
    p->udp_tcp_peer=peer;p->udp_tcp_peer_len=peer_len;
    p->down_pool_id=0;
    if(epoll_tcp_update(p,1)!=0){log4c_warn("entry id=%u epoll add fail: %s",p->id,strerror(errno));ps_free(p);return 1;}
    if(G.socks_enabled){
        p->socks_stage=0; /* 等 SOCKS5 握手拿到 target 后再开 QUIC stream */
        log4c_debug("entry id=%u accept fd=%d (socks, awaiting handshake)",p->id,fd);
        return 1;
    }
    snprintf(p->route,sizeof(p->route),"%s",G.route_str);
    { nb_flow_policy_t pol; char thost[256]="-"; int tport=0;
      int parsed = (parse_target_from_route(G.route_str, thost, sizeof(thost), &tport) == 0);
      if(parsed) flow_policy_for_host(thost, tport, &pol);
      else nb_flow_policy_default(&pol);
      int prio = pol.prio;
      log4c_info("entry id=%u classify fixed route=%s host=%s port=%d class=%s lane=%s fec=%s rule=%s prio=%d",
        p->id, G.route_str, parsed ? thost : "-", parsed ? tport : 0,
        nb_flow_class_name(pol.flow_class), nb_flow_lane_name(pol.lane_hint), nb_flow_fec_name(pol.fec_hint), pol.rule_name, prio);
      if(downstream_open_stream(p,G.route_str,&pol)!=0){ ps_free(p); return 1; } }
    log4c_info("entry id=%u accept fd=%d -> down_sid=%llu route=%s",p->id,fd,(unsigned long long)p->down_stream_id,G.route_str);
    return 1;
}

static void entry_accept(void){
    /* Shadowrocket latency checks arrive as a burst. Drain the level-triggered
     * listen queue so short probes do not wait behind a full event-loop turn. */
    for(int accepted=0;accepted<64;accepted++){
        if(!entry_accept_one())break;
    }
}

static int socks_auth_finish(proxy_stream_t* p,const char* username,int credential_valid,
    int cache_hit,uint64_t worker_us){
    int tenant=g_tenants_enabled?nb_tenant_find(&g_tenants,username):-1;
    int valid=credential_valid&&(!g_tenants_enabled||tenant>=0);
    uint8_t response[2]={1,valid?0:1};(void)send(p->tcp_fd,response,2,MSG_NOSIGNAL);
    if(!valid){
        log4c_warn("entry id=%u socks authentication failed peer=%s user=%s worker=%.1fms",
            p->id,p->peer_addr,username,worker_us/1000.0);
        ps_set_close_reason(p,"socks-auth-failed");ps_free(p);return 0;
    }
    p->tenant_index=tenant;snprintf(p->tenant_name,sizeof(p->tenant_name),"%s",username);
    p->socks_auth_worker_us=worker_us;p->socks_auth_at=picoquic_current_time();p->socks_stage=2;
    log4c_debug("entry id=%u socks authenticated user=%s cache=%s worker=%.1fms",
        p->id,username,cache_hit?"hit":"miss",worker_us/1000.0);
    if(epoll_tcp_update(p,0)!=0){ps_set_close_reason(p,"socks-auth-epoll");ps_free(p);return 0;}
    return 1;
}

/* entry SOCKS5 入口: 推进握手, 完成后用动态 target 开 QUIC stream。 */
static void socks_handshake(proxy_stream_t* p){
    if(p->socks_auth_pending)return;
    uint8_t buf[512];
    size_t buffered=p->socks_len;
    ssize_t n=recv(p->tcp_fd,buf,sizeof(buf),0);
    if(n<=0){ /* 已缓存流水线 request 时，EAGAIN 仍继续解析现有缓冲。 */
        if(n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)) ps_teardown(p);
        if(buffered==0||n==0)return;
    }else{
        if(p->socks_len+(size_t)n>sizeof(p->socks_buf)){ log4c_warn("id=%u socks buf overflow",p->id); ps_free(p); return; }
        memcpy(p->socks_buf+p->socks_len,buf,(size_t)n); p->socks_len+=(size_t)n;
    }

    if(p->socks_stage==0){ /* greeting: VER NMETHODS METHODS... */
        if(p->socks_len<2) return;
        if(p->socks_buf[0]!=0x05){ log4c_warn("id=%u not socks5 (ver=%u)",p->id,p->socks_buf[0]); ps_free(p); return; }
        uint8_t nm=p->socks_buf[1];
        if(p->socks_len<(size_t)(2+nm)) return; /* 等齐 */
        uint8_t wanted=g_socks_auth_enabled?0x02:0x00,method=0xff;
        for(uint8_t i=0;i<nm;i++)if(p->socks_buf[2+i]==wanted){method=wanted;break;}
        char methods[128];
        socks_methods_to_text(p->socks_buf + 2, nm, methods, sizeof(methods));
        uint8_t rep[2]={0x05,method};
        (void)send(p->tcp_fd,rep,2,MSG_NOSIGNAL);
        size_t used=2+nm; memmove(p->socks_buf,p->socks_buf+used,p->socks_len-used); p->socks_len-=used;
        if(method==0xff){
            log4c_warn("entry id=%u socks reject peer=%s offered=[%s] required=0x%02x reason=no-acceptable-auth-method",
                p->id,p->peer_addr,methods,wanted);
            ps_set_close_reason(p,"socks-auth-method");ps_free(p);return;
        }
        p->socks_stage=g_socks_auth_enabled?1:2;
        p->socks_greeting_at=picoquic_current_time();
    }
    if(p->socks_stage==1){ /* RFC 1929: VER ULEN UNAME PLEN PASSWD */
        if(p->socks_len<2)return;
        if(p->socks_buf[0]!=0x01){ps_free(p);return;}
        size_t ulen=p->socks_buf[1];
        if(ulen==0||ulen>=NB_AUTH_NAME_MAX){uint8_t r[2]={1,1};(void)send(p->tcp_fd,r,2,MSG_NOSIGNAL);ps_free(p);return;}
        if(p->socks_len<2+ulen+1)return;
        size_t plen=p->socks_buf[2+ulen],used=3+ulen+plen;
        if(plen==0||p->socks_len<used)return;
        char username[NB_AUTH_NAME_MAX];memcpy(username,p->socks_buf+2,ulen);username[ulen]=0;
        unsigned char fingerprint[NB_AUTH_FINGERPRINT_LEN];uint64_t auth_now=picoquic_current_time();
        int fingerprint_ok=nb_auth_fingerprint(&g_socks_users,username,p->socks_buf+3+ulen,plen,fingerprint);
        int cache_hit=fingerprint_ok&&nb_auth_cache_lookup(&g_socks_users,fingerprint,auth_now);
        int coalesced=0;
        if(!cache_hit&&fingerprint_ok)for(int i=0;i<MAX_CONN;i++){
            proxy_stream_t* pending=&G.streams[i];
            if(pending!=p&&pending->in_use&&pending->socks_auth_pending&&
                memcmp(pending->socks_auth_fingerprint,fingerprint,sizeof(fingerprint))==0){coalesced=1;break;}
        }
        int submitted=(cache_hit||coalesced)?0:nb_auth_async_context_submit(
            nb_current_instance->auth_async,p->id,username,p->socks_buf+3+ulen,plen);
        memset(p->socks_buf+3+ulen,0,plen);
        memmove(p->socks_buf,p->socks_buf+used,p->socks_len-used);p->socks_len-=used;
        if(cache_hit){if(!socks_auth_finish(p,username,1,1,0))return;}
        else if(submitted==0){
            p->socks_auth_pending=1;snprintf(p->socks_auth_username,sizeof(p->socks_auth_username),"%s",username);
            memcpy(p->socks_auth_fingerprint,fingerprint,sizeof(fingerprint));
            memset(fingerprint,0,sizeof(fingerprint));
            if(coalesced)log4c_debug("entry id=%u socks authentication coalesced user=%s",p->id,username);
            if(epoll_tcp_update(p,0)!=0){ps_set_close_reason(p,"socks-auth-epoll");ps_free(p);}return;
        }else{
            memset(fingerprint,0,sizeof(fingerprint));
            log4c_warn("entry id=%u socks authentication queue unavailable peer=%s user=%s",p->id,p->peer_addr,username);
            (void)socks_auth_finish(p,username,0,0,0);return;
        }
        memset(fingerprint,0,sizeof(fingerprint));
    }
    if(p->socks_stage==2){ /* request: VER CMD RSV ATYP ADDR PORT */
        if(p->socks_len<4) return;
        uint8_t cmd=p->socks_buf[1], atyp=p->socks_buf[3];
        char host[256]={0}; int port=0; size_t need=0;
        if(atyp==0x01){ need=4+4+2; if(p->socks_len<need) return;
            snprintf(host,sizeof(host),"%u.%u.%u.%u",p->socks_buf[4],p->socks_buf[5],p->socks_buf[6],p->socks_buf[7]);
            port=(p->socks_buf[8]<<8)|p->socks_buf[9]; }
        else if(atyp==0x03){ uint8_t dl=p->socks_buf[4]; need=4+1+(size_t)dl+2; if(p->socks_len<need) return;
            memcpy(host,p->socks_buf+5,dl); host[dl]=0; port=(p->socks_buf[5+dl]<<8)|p->socks_buf[6+dl]; }
        else if(atyp==0x04){ need=4+16+2; if(p->socks_len<need) return;
            inet_ntop(AF_INET6,p->socks_buf+4,host,sizeof(host)); port=(p->socks_buf[20]<<8)|p->socks_buf[21]; }
        else {
            log4c_warn("entry id=%u socks request peer=%s cmd=%s(0x%02x) atyp=%s(0x%02x) result=reject reason=unsupported-atyp",
                p->id,p->peer_addr,socks_cmd_name(cmd),cmd,socks_atyp_name(atyp),atyp);
            uint8_t r[10]={0x05,0x08,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
            ps_set_close_reason(p,"socks-unsupported-atyp");ps_free(p);return;
        }
        p->socks_request_at=picoquic_current_time();
        log4c_info("entry id=%u socks request peer=%s cmd=%s(0x%02x) atyp=%s(0x%02x) target=%s:%d accept_age=%.1fms socks_ms=greeting:%.1f,auth:%.1f,request:%.1f",
            p->id,p->peer_addr,socks_cmd_name(cmd),cmd,socks_atyp_name(atyp),atyp,host,port,
            trace_delta_ms(p->socks_request_at,p->created_at),
            trace_delta_ms(p->socks_greeting_at,p->created_at),
            trace_delta_ms(p->socks_auth_at,p->socks_greeting_at),
            trace_delta_ms(p->socks_request_at,p->socks_auth_at?p->socks_auth_at:p->socks_greeting_at));
        if(cmd==0x03){
            if(g_tenants_enabled&&nb_tenant_acquire(&g_tenants,p->tenant_index,1,picoquic_current_time())!=0){uint8_t r[10]={5,2,0,1,0,0,0,0,0,0};(void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);ps_set_close_reason(p,"tenant-udp-quota");ps_free(p);return;}p->tenant_acquired=g_tenants_enabled;p->tenant_udp=1;
            struct sockaddr_in bind_addr;int ufd=socks_udp_bind_socket(&bind_addr);
            if(ufd<0){
                int saved=errno;if(ufd>=0)close(ufd);
                log4c_warn("entry id=%u UDP ASSOCIATE bind fail peer=%s range=%u-%u error=%s",
                    p->id,p->peer_addr,g_socks_udp_port_min,g_socks_udp_port_max,strerror(saved));
                uint8_t r[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};(void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
                ps_set_close_reason(p,"udp-associate-bind-fail");ps_free(p);return;
            }
            p->udp_mode=1;p->udp_association=1;p->udp_fd=ufd;p->socks_stage=3;
            if(epoll_udp_update(p,1)!=0){
                uint8_t r[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};(void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
                ps_set_close_reason(p,"udp-associate-epoll-fail");ps_free(p);return;
            }
            uint16_t relay_port=ntohs(bind_addr.sin_port);struct in_addr relay_addr={0};
            if(g_socks_udp_advertise_configured)relay_addr=g_socks_udp_advertise_addr;
            else {struct sockaddr_in local;socklen_t local_len=sizeof(local);memset(&local,0,sizeof(local));
                if(getsockname(p->tcp_fd,(struct sockaddr*)&local,&local_len)==0&&local.sin_family==AF_INET)
                    relay_addr=local.sin_addr;}
            if(relay_addr.s_addr==INADDR_ANY){
                log4c_warn("entry id=%u UDP ASSOCIATE has no reachable advertise address",p->id);
                uint8_t fail[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};(void)send(p->tcp_fd,fail,10,MSG_NOSIGNAL);
                ps_set_close_reason(p,"udp-associate-no-address");ps_free(p);return;
            }
            uint8_t r[10]={0x05,0x00,0,0x01,0,0,0,0,(uint8_t)(relay_port>>8),(uint8_t)relay_port};
            memcpy(r+4,&relay_addr,sizeof(relay_addr));
            (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
            char relay_ip[INET_ADDRSTRLEN];inet_ntop(AF_INET,&relay_addr,relay_ip,sizeof(relay_ip));
            log4c_info("entry id=%u UDP ASSOCIATE ready peer=%s relay=%s:%u accept_age=%.1fms request_to_ready=%.1fms",
                p->id,p->peer_addr,relay_ip,relay_port,trace_delta_ms(picoquic_current_time(),p->created_at),
                trace_delta_ms(picoquic_current_time(),p->socks_request_at));
            memmove(p->socks_buf,p->socks_buf+need,p->socks_len-need);p->socks_len-=need;
            return;
        }
        if(cmd!=0x01){ /* 仅支持 CONNECT */
            log4c_warn("entry id=%u socks request peer=%s cmd=%s(0x%02x) atyp=%s(0x%02x) target=%s:%d result=reject reason=unsupported-command",
                p->id,p->peer_addr,socks_cmd_name(cmd),cmd,socks_atyp_name(atyp),atyp,host,port);
            uint8_t r[10]={0x05,0x07,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
            ps_set_close_reason(p,"socks-unsupported-command");ps_free(p);return; }
        if(g_tenants_enabled&&nb_tenant_acquire(&g_tenants,p->tenant_index,0,picoquic_current_time())!=0){uint8_t r[10]={5,2,0,1,0,0,0,0,0,0};(void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);ps_set_close_reason(p,"tenant-tcp-quota");ps_free(p);return;}p->tenant_acquired=g_tenants_enabled;
        if(!whitelist_allowed(host,port)){ /* 白名单外 -> 拒绝(0x02 not allowed by ruleset), 不占三跳线路 */
            log4c_debug("entry id=%u BLOCKED %s:%d (not in whitelist)",p->id,host,port);
            uint8_t r[10]={0x05,0x02,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL); ps_free(p); return; }
        const nb_route_entry_t* selected=G.exit_routes_enabled?nb_routes_pick_key(&G.exit_routes,nb_routes_hash(p->tenant_name,host,(uint16_t)port),picoquic_current_time()):NULL;
        if(G.exit_routes_enabled&&!selected){uint8_t r[10]={5,1,0,1,0,0,0,0,0,0};(void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);ps_set_close_reason(p,"no-healthy-route");ps_free(p);return;}if(selected){p->route_index=nb_routes_acquire(&G.exit_routes,selected);if(p->route_index<0){ps_free(p);return;}}
        if(port==NB_PROBE_PORT&&(!strcasecmp(host,NB_PROBE_SINK_HOST)||!strcasecmp(host,NB_PROBE_ECHO_HOST)||
            !strcasecmp(host,NB_PROBE_SOURCE_HOST)))
            p->probe_mode=3; /* entry 等待探针响应 FIN，不能在客户端半关闭后立即拆流 */
        uint8_t rep[10]={0x05,0x00,0x00,0x01,0,0,0,0,0,0}; /* 成功, BND 全 0 */
        (void)send(p->tcp_fd,rep,10,MSG_NOSIGNAL);
        memmove(p->socks_buf,p->socks_buf+need,p->socks_len-need); p->socks_len-=need;
        /* 构造 route: 中间跳前缀(可空) + 动态 target */
        char route[300];
        const char* mid=selected?selected->hop:G.mid_route;
        int route_len=mid[0]
            ? snprintf(route,sizeof(route),"%s,T:%s:%d",mid,host,port)
            : snprintf(route,sizeof(route),"T:%s:%d",host,port);
        if(route_len<0||(size_t)route_len>=sizeof(route)){
            log4c_warn("entry id=%u route too long host=%s",p->id,host);
            uint8_t r[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};
            (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);ps_free(p);return;
        }
        snprintf(p->route,sizeof(p->route),"%s",route);
        nb_flow_policy_t pol; flow_policy_for_host(host, port, &pol);
        int prio=pol.prio;
        log4c_info("entry id=%u classify peer=%s host=%s port=%d class=%s lane=%s fec=%s rule=%s prio=%d route=%s",
            p->id, p->peer_addr, host, port, nb_flow_class_name(pol.flow_class), nb_flow_lane_name(pol.lane_hint),
            nb_flow_fec_name(pol.fec_hint), pol.rule_name, prio, route);
        if(selected)log4c_debug("entry id=%u selected exit route=%s hop=%s",p->id,selected->name,selected->hop);
        if(downstream_open_stream(p,route,&pol)!=0){ ps_free(p); return; }
        p->socks_stage=3;
        log4c_debug("entry id=%u socks CONNECT %s:%d -> down_sid=%llu route=%s",p->id,host,port,(unsigned long long)p->down_stream_id,route);
        /* request 之后可能已跟随应用数据 -> 转发到下游 */
        if(p->socks_len>0){ fwd_down(p,p->socks_buf,p->socks_len,0); p->socks_len=0; }
    }
}

static void flush_q2t(proxy_stream_t* p){
    if(p->tcp_fd<0) return;
    while(p->q2t.len>0){
        const uint8_t* data=NULL;size_t avail=nb_ring_peek(&p->q2t,&data);
        ssize_t n=send(p->tcp_fd,data,avail,MSG_NOSIGNAL);
        if(n>0){
            if(p->first_q2t_flush_at==0){
                p->first_q2t_flush_at=picoquic_current_time();
                if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=%s age=%.1fms queue_wait=%.1fms bytes=%zd q_before=%zu route=%s",
                    role_name(G.role),p->id,G.role==ROLE_EXIT?"target-write":"client-write",
                    trace_delta_ms(p->first_q2t_flush_at,p->created_at),
                    trace_delta_ms(p->first_q2t_flush_at,p->first_q2t_queue_at),n,p->q2t.len,p->route);
            }
            nb_send_consume(&p->q2t,&p->queue_clocks->q2t,(size_t)n);
            logical_note_delivered(p,(size_t)n);ps_touch(p);
        }
        else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK){
            /* 本地 TCP 已死(EPIPE/ECONNRESET/...): 待写数据无处可去, 丢弃并标记 EOF -> 触发回收,
             * 否则 q2t 恒非空使 maybe_free 永不满足, 流泄漏。 */
            log4c_debug("id=%u q2t send err=%s, drop %zu & teardown",p->id,strerror(errno),p->q2t.len);
            ps_set_close_reason(p,"tcp-send-error");
            p->tcp_eof=1; nb_send_clear(&p->q2t,&p->queue_clocks->q2t);
            break;
        }
        else break; /* EAGAIN: 等下轮可写 */
    }
    if(p->q2t.len==0 && p->q2t_fin){
        shutdown(p->tcp_fd,SHUT_WR);p->q2t_fin=0;logical_note_fin_delivered(p);
        if(G.role==ROLE_ENTRY&&p->probe_mode==3){ps_teardown_reason(p,"probe-complete");return;}
    }
}

static int tcp_read_allowed(proxy_stream_t* p){
    size_t main_q=(G.role==ROLE_ENTRY)?p->down_tx.len:p->up_tx.len;
    nb_live_queue_clock_t* main_clock=(G.role==ROLE_ENTRY)?&p->queue_clocks->down_tx:&p->queue_clocks->up_tx;
    size_t stage_q=p->fec_stage_tx_len;
    size_t dg_q=p->fec_dg_tx_len;
    size_t replay_q=p->logical_endpoint?p->logical_tx.replay_length:0;
    uint64_t now=picoquic_current_time();
    if(p->tenant_throttled_until>now)return 0;
    p->tenant_throttled_until=0;
    nb_live_queue_limits_t limits=nb_live_queue_limits(p->flow_class,p->udp_mode);
    uint64_t queue_age=nb_live_queue_age_us(main_clock,main_q,now);
    int age_limited=p->flow_class==NB_FLOW_CLASS_MEDIA;
    if(p->tcp_read_paused){
        if(main_q<=limits.low_bytes&&(!age_limited||queue_age<limits.deadline_us)&&
            stage_q<=limits.low_bytes&&dg_q<=NB_FEC_DG_LOW&&replay_q<=limits.low_bytes){
            p->tcp_read_paused=0;
            log4c_info("%s tcp read resumed id=%u class=%s main=%zu age=%.1fms low=%zu route=%s",
                role_name(G.role),p->id,nb_flow_class_name(p->flow_class),main_q,
                queue_age/1000.0,limits.low_bytes,p->route);
        }
    }else if(main_q>=limits.high_bytes||(age_limited&&queue_age>=limits.deadline_us)||
        stage_q>=limits.high_bytes||dg_q>=NB_FEC_DG_HIGH||replay_q>=NB_LSTREAM_REPLAY_MAX){
        p->tcp_read_paused=1;
        log4c_info("%s tcp read paused id=%u class=%s main=%zu age=%.1fms stage=%zu dg=%zu high=%zu route=%s",
            role_name(G.role),p->id,nb_flow_class_name(p->flow_class),main_q,queue_age/1000.0,
            stage_q,dg_q,limits.high_bytes,p->route);
    }
    return !p->tcp_read_paused;
}

static int epoll_tcp_update(proxy_stream_t* p,int add){
    if(G.epoll_fd<0||p==NULL||!p->in_use||p->tcp_fd<0)return -1;
    uint32_t events=EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    if(G.socks_enabled&&p->socks_stage<3){
        if(!p->socks_auth_pending)events|=EPOLLIN;
    }else if(p->tcp_connecting||(!p->tcp_eof&&tcp_read_allowed(p)))events|=EPOLLIN;
    if(nb_send_local_write_pending(p->q2t.len,p->tcp_connecting,p->q2t_fin))events|=EPOLLOUT;
    size_t index=(size_t)(p-G.streams);if(index>=MAX_CONN)return -1;
    struct epoll_event ev;memset(&ev,0,sizeof(ev));ev.events=events;
    ev.data.u64=NB_EPOLL_TAG_STREAM|((uint64_t)p->id<<16)|(uint64_t)index;
    int op=add?EPOLL_CTL_ADD:EPOLL_CTL_MOD;
    if(epoll_ctl(G.epoll_fd,op,p->tcp_fd,&ev)==0)return 0;
    if(!add&&errno==ENOENT)return epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,p->tcp_fd,&ev);
    return -1;
}

static int epoll_udp_update(proxy_stream_t* p,int add){
    if(G.epoll_fd<0||p==NULL||!p->in_use||p->udp_fd<0)return -1;
    size_t index=(size_t)(p-G.streams);if(index>=MAX_CONN)return -1;
    struct epoll_event ev;memset(&ev,0,sizeof(ev));ev.events=EPOLLIN|EPOLLERR;
    ev.data.u64=NB_EPOLL_TAG_UDP_SESSION|((uint64_t)p->id<<16)|(uint64_t)index;
    return epoll_ctl(G.epoll_fd,add?EPOLL_CTL_ADD:EPOLL_CTL_MOD,p->udp_fd,&ev);
}

static proxy_stream_t* udp_entry_child_get(proxy_stream_t* parent,const char* host,int port,const char** reject_reason){
    if(reject_reason!=NULL)*reject_reason=NULL;
    proxy_stream_t* p=ps_find_udp_child(parent->id,host,port);if(p!=NULL)return p;
    if(!whitelist_allowed(host,port)){if(reject_reason!=NULL)*reject_reason="policy";return NULL;}
    p=ps_alloc();if(p==NULL){if(reject_reason!=NULL)*reject_reason="session-capacity";return NULL;}p->udp_mode=1;p->udp_parent_id=parent->id;
    p->tenant_index=parent->tenant_index;snprintf(p->tenant_name,sizeof(p->tenant_name),"%s",parent->tenant_name);
    p->udp_session_id=udp_session_id_new();p->down_pool_id=0;
    if(p->udp_session_id==0||G.pool_count<=0||udp_reassembly_ensure(p)!=0){if(reject_reason!=NULL)*reject_reason="child-init";ps_set_close_reason(p,"udp-child-init-fail");ps_free(p);return NULL;}
    snprintf(p->udp_target_host,sizeof(p->udp_target_host),"%s",host);p->udp_target_port=port;
    ps_index_refresh(p);
    const nb_route_entry_t* selected=G.exit_routes_enabled?nb_routes_pick_key(&G.exit_routes,nb_routes_hash(parent->tenant_name,host,(uint16_t)port),picoquic_current_time()):NULL;
    if(G.exit_routes_enabled&&!selected){if(reject_reason!=NULL)*reject_reason="no-healthy-route";ps_set_close_reason(p,"udp-no-healthy-route");ps_free(p);return NULL;}if(selected){p->route_index=nb_routes_acquire(&G.exit_routes,selected);if(p->route_index<0){if(reject_reason!=NULL)*reject_reason="route-capacity";ps_set_close_reason(p,"udp-route-capacity");ps_free(p);return NULL;}}
    const char* mid=selected?selected->hop:G.mid_route;
    int n=mid[0]?snprintf(p->route,sizeof(p->route),"%s,T:%s:%d",mid,host,port):
        snprintf(p->route,sizeof(p->route),"T:%s:%d",host,port);
    if(n<=0||(size_t)n>=sizeof(p->route)){if(reject_reason!=NULL)*reject_reason="route-overflow";ps_set_close_reason(p,"udp-route-overflow");ps_free(p);return NULL;}
    nb_flow_policy_t pol;flow_policy_for_host(host,port,&pol);apply_flow_policy(p,&pol);
    cnx_pool_t* pool=&G.pools[0];int idx=pool_pick(pool,pol.lane_hint==NB_FLOW_LANE_LATENCY);
    if(idx<0){if(reject_reason!=NULL)*reject_reason="no-downstream";ps_set_close_reason(p,"udp-no-downstream");ps_free(p);return NULL;}
    p->down_cnx=pool->cnx[idx];p->down_pool_idx=idx;
    ps_index_refresh(p);
    if(pol.flow_class==NB_FLOW_CLASS_MEDIA)picoquic_set_media_mode(p->down_cnx,1);
    log4c_info("entry udp flow open id=%u parent=%u usid=%u peer=%s target=%s:%d class=%s route=%s",
        p->id,parent->id,p->udp_session_id,parent->peer_addr,host,port,nb_flow_class_name(pol.flow_class),p->route);
    return p;
}

static void udp_rxq_overflow_note(const char* path,uint32_t stream_id,uint64_t dropped){
    if(dropped==0)return;
    nb_metrics_note_udp_rxq_overflow(&g_metrics_state,dropped);
    uint64_t now=picoquic_current_time();
    if(now-nb_current_instance->rxq_last_warn_at>=1000000ULL){nb_current_instance->rxq_last_warn_at=now;
        log4c_warn("udp receive queue overflow path=%s id=%u dropped=%llu",
            path,stream_id,(unsigned long long)dropped);}
}

static void udp_local_drain(proxy_stream_t* p){
    uint8_t buffer[65535+300];
    for(;;){
        if(p->udp_association){
            struct sockaddr_storage from;socklen_t from_len=sizeof(from);
            uint64_t dropped=0;ssize_t n=nb_udp_recv(p->udp_fd,buffer,sizeof(buffer),0,
                (struct sockaddr*)&from,&from_len,&p->udp_rxq_state,&dropped);
            udp_rxq_overflow_note("socks-relay",p->id,dropped);
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
            if(n<=0){if(n<0)log4c_warn("entry UDP relay recv fail id=%u error=%s",p->id,strerror(errno));break;}
            p->udp_assoc_raw_rx++;
            if(p->udp_assoc_first_raw_at==0){
                p->udp_assoc_first_raw_at=picoquic_current_time();
                char text[96];sockaddr_to_text((struct sockaddr*)&from,from_len,text,sizeof(text));
                log4c_info("entry UDP ASSOCIATE first raw id=%u source=%s tcp_peer=%s bytes=%zd age=%.1fms",
                    p->id,text,p->peer_addr,n,trace_delta_ms(p->udp_assoc_first_raw_at,p->created_at));
            }
            if(!sockaddr_ip_equal(&from,&p->udp_tcp_peer)){
                p->udp_assoc_reject_ip++;
                char text[96];sockaddr_to_text((struct sockaddr*)&from,from_len,text,sizeof(text));
                log4c_warn("entry UDP relay reject id=%u source=%s tcp_peer=%s",p->id,text,p->peer_addr);continue;
            }
            if(!p->udp_client_peer_set){p->udp_client_peer=from;p->udp_client_peer_len=from_len;p->udp_client_peer_set=1;}
            else if(!sockaddr_ip_equal(&from,&p->udp_client_peer))continue;
            char host[256];int port=0;const uint8_t* payload=NULL;size_t payload_len=0;
            if(nb_socks_udp_parse(buffer,(size_t)n,host,sizeof(host),&port,&payload,&payload_len)!=0){
                p->udp_assoc_malformed++;
                log4c_warn("entry UDP relay malformed id=%u bytes=%zd",p->id,n);continue;
            }
            const char* reject_reason=NULL;
            proxy_stream_t* child=udp_entry_child_get(p,host,port,&reject_reason);if(child==NULL){
                if(reject_reason!=NULL&&!strcmp(reject_reason,"policy"))p->udp_assoc_policy_drop++;
                log4c_warn("entry UDP relay drop id=%u target=%s:%d reason=%s",p->id,host,port,reject_reason?reject_reason:"unknown");
                continue;
            }
            if(child->udp_first_local_c2s_at==0){
                child->udp_first_local_c2s_at=picoquic_current_time();
                log4c_info("entry udp trace id=%u usid=%u stage=phone-first-rx association_age=%.1fms flow_age=%.1fms bytes=%zu target=%s:%d",
                    child->id,child->udp_session_id,trace_delta_ms(child->udp_first_local_c2s_at,p->created_at),
                    trace_delta_ms(child->udp_first_local_c2s_at,child->created_at),payload_len,host,port);
            }
            int queue_rc=udp_queue_packet(child,1,NB_UDP_TYPE_C2S,child->route,payload,payload_len);
            if(queue_rc<0){
                ps_teardown_reason(child,"udp-down-queue-fail");continue;
            }
            if(queue_rc>0)continue;
            child->udp_packets_c2s++;child->bytes_c2s+=payload_len;ps_touch(child);ps_touch(p);
        }else if(G.role==ROLE_EXIT){
            uint64_t dropped=0;ssize_t n=nb_udp_recv(p->udp_fd,buffer,NB_UDP_MAX_PAYLOAD,0,
                NULL,NULL,&p->udp_rxq_state,&dropped);
            udp_rxq_overflow_note("exit-target",p->id,dropped);
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
            if(n<=0){if(n<0)log4c_warn("exit UDP recv fail id=%u error=%s",p->id,strerror(errno));break;}
            if(p->udp_first_target_rx_at==0){
                p->udp_first_target_rx_at=picoquic_current_time();
                log4c_info("exit udp trace id=%u usid=%u stage=target-first-rx age=%.1fms send_to_rx=%.1fms bytes=%zd target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_target_rx_at,p->created_at),
                    trace_delta_ms(p->udp_first_target_rx_at,p->udp_first_target_send_at),n,
                    p->udp_target_host,p->udp_target_port);
            }
            int queue_rc=udp_queue_packet(p,0,NB_UDP_TYPE_S2C,p->route,buffer,(size_t)n);
            if(queue_rc<0){
                ps_teardown_reason(p,"udp-up-queue-fail");break;
            }
            if(queue_rc>0)continue;
            p->udp_packets_s2c++;p->bytes_s2c+=(size_t)n;ps_touch(p);
        }else break;
    }
}

static void pump_tcp(proxy_stream_t* p){
    if(p==NULL||p->tcp_eof)return;
    uint8_t buf[16384]; ssize_t n;
    for(;;){
        size_t desired=sizeof(buf);
        if(p->logical_endpoint){
            if(p->logical_tx.replay_length>=NB_LSTREAM_REPLAY_MAX){(void)tcp_read_allowed(p);break;}
            size_t replay_left=NB_LSTREAM_REPLAY_MAX-p->logical_tx.replay_length;
            if(desired>replay_left)desired=replay_left;
        }
        int direction=G.role==ROLE_ENTRY?NB_TENANT_UP:NB_TENANT_DOWN;
        uint64_t now=picoquic_current_time();
        int media=p->flow_class==NB_FLOW_CLASS_MEDIA&&!p->downlink_bulk;
        size_t limit=g_tenants_enabled?nb_tenant_take_class(&g_tenants,p->tenant_index,
            desired,now,direction,media):desired;
        if(limit==0){size_t retry_bytes=media?1:desired;
            p->tenant_throttled_until=now+nb_tenant_retry_after_us(&g_tenants,p->tenant_index,retry_bytes,direction);break;}
        n=recv(p->tcp_fd,buf,limit,0);
        if(n>0){
            if(g_tenants_enabled&&(size_t)n<limit)nb_tenant_refund(&g_tenants,p->tenant_index,limit-(size_t)n,direction);
            if(G.role==ROLE_ENTRY) fwd_down(p,buf,(size_t)n,0); else fwd_up(p,buf,(size_t)n,0);
            if(p->need_teardown||!tcp_read_allowed(p))break;
        }
        else if(n==0){if(g_tenants_enabled)nb_tenant_refund(&g_tenants,p->tenant_index,limit,direction);p->tcp_eof=1;ps_set_close_reason(p,G.role==ROLE_ENTRY?"client-eof":"target-eof");
            if(G.role==ROLE_ENTRY) fwd_down(p,NULL,0,1); else fwd_up(p,NULL,0,1);
            break; }
        else if(errno==EAGAIN||errno==EWOULDBLOCK){if(g_tenants_enabled)nb_tenant_refund(&g_tenants,p->tenant_index,limit,direction);break;}
        else{
            if(g_tenants_enabled)nb_tenant_refund(&g_tenants,p->tenant_index,limit,direction);
            log4c_debug("id=%u tcp recv err=%s",p->id,strerror(errno));p->tcp_eof=1;
            ps_set_close_reason(p,"tcp-recv-error");
            if(G.role==ROLE_ENTRY)fwd_down(p,NULL,0,1);else fwd_up(p,NULL,0,1);break;
        }
    }
}

/* 判定并释放已完成的流。
 * 关键: 真实服务器(HTTP keep-alive)响应完不主动关 TCP, 若死等"双向 FIN 齐全"则流永不回收 -> 泄漏。
 * 但反向的坑更隐蔽: exit 侧回程响应位于 up_tx 发送环中(不占 q2t),
 * target 一关(tcp_eof)队列往往还没发完; 若此时 discard 会截断回程(大文件只传一部分)。故:
 *   1) 双向数据方向都结束 且 q2t 排空 -> 优雅回收;
 *   2) 仅 ENTRY: client TCP 关闭即事务结束, 回程无处可送, 主动 teardown(丢回程 OK, 且连锁 reset 清下游);
 *   3) EXIT 不主动 teardown: 必须让上游 stream 把排队响应发完(靠 client 收完关闭触发的去程 FIN
 *      或 entry 的连锁 reset 或空闲超时来收尾), 否则截断大流量回程。 */
static void maybe_free(proxy_stream_t* p){
    int left_done  = (G.role==ROLE_ENTRY)? p->tcp_eof : p->up_fin_seen;
    int right_done = (G.role==ROLE_EXIT) ? p->tcp_eof : p->down_fin_seen;
    size_t prefix_left=(p->down_prefix_len>=p->down_prefix_off)
        ?p->down_prefix_len-p->down_prefix_off:0;
    int reliable_pending=prefix_left>0||p->down_tx.len>0||p->up_tx.len>0||p->q2t.len>0||
        p->down_tx_fin||p->up_tx_fin||p->fec_stage_tx_len>0||p->fec_stage_tx_fin||p->fec_dg_tx_len>0||
        (p->fec_sidecar_mode&&p->fec_engine!=NULL&&!nb_fec_tx_complete(p->fec_engine))||
        (p->logical_endpoint&&(p->logical_tx.replay_length>0||
            (p->logical_tx.fin_queued&&!p->logical_tx.fin_acked)));
    if(left_done && right_done && !reliable_pending){ ps_teardown_reason(p,"bidirectional-fin"); return; }
    if(G.role==ROLE_ENTRY && p->tcp_eof && p->q2t.len==0 && p->probe_mode==0&&
        !p->logical_endpoint){
        ps_teardown_reason(p,"entry-client-eof"); return;
    }
}

/* P1-2a: recvmmsg 批量收 UDP -> 降低高包率(大流量)下的接收 syscall 开销。
 * 每个 epoll 轮次只收一个有界批次。level-triggered epoll 会在下一轮继续
 * 通知；轮次之间必须让 udp_send_batch() 运行，避免长 RTT 入站突发在发送
 * 获得机会前灌满低延迟媒体队列。 */
#define UDP_RECV_BATCH 32
static void udp_drain_fd(int fd,const struct sockaddr_storage* local){
    static _Thread_local struct mmsghdr msgs[UDP_RECV_BATCH];
    static _Thread_local struct iovec iovs[UDP_RECV_BATCH];
    static _Thread_local uint8_t bufs[UDP_RECV_BATCH][1536];
    static _Thread_local struct sockaddr_storage froms[UDP_RECV_BATCH];
    static _Thread_local union { struct cmsghdr align;uint8_t bytes[CMSG_SPACE(sizeof(uint32_t))]; }
        controls[UDP_RECV_BATCH];
    static _Thread_local int inited=0;
    if(!inited){
        for(int i=0;i<UDP_RECV_BATCH;i++){
            iovs[i].iov_base=bufs[i]; iovs[i].iov_len=sizeof(bufs[i]);
            msgs[i].msg_hdr.msg_iov=&iovs[i]; msgs[i].msg_hdr.msg_iovlen=1;
            msgs[i].msg_hdr.msg_name=&froms[i];msgs[i].msg_hdr.msg_control=controls[i].bytes;
            msgs[i].msg_hdr.msg_controllen=sizeof(controls[i].bytes);
        }
        inited=1;
    }
    for(int i=0;i<UDP_RECV_BATCH;i++){
        msgs[i].msg_hdr.msg_controllen=sizeof(controls[i].bytes);
        msgs[i].msg_hdr.msg_flags=0;
        msgs[i].msg_hdr.msg_namelen=sizeof(froms[i]); /* namelen 是 in/out, 每次重置 */
    }
    int got=recvmmsg(fd,msgs,UDP_RECV_BATCH,0,NULL);
    if(got<=0){
        if(got<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)
            nb_metrics_note_udp_error(&g_metrics_state,0);
        return;
    }
    nb_udp_rxq_state_t* rxq_state=fd==G.client_udp_fd?&g_udp_rxq_client:&g_udp_rxq_main;
    for(int m=0;m<got;m++){
        uint64_t dropped=nb_udp_rxq_overflow_update(rxq_state,&msgs[m].msg_hdr);
        udp_rxq_overflow_note(fd==G.client_udp_fd?"middle-client-quic":"quic",0,dropped);
        uint64_t packet_now=picoquic_current_time();
        picoquic_incoming_packet(G.quic,bufs[m],(size_t)msgs[m].msg_len,
            (struct sockaddr*)&froms[m],(struct sockaddr*)local,0,0,packet_now);
    }
}

/* P1-2b: UDP GSO 批量发送。picoquic 逐包 prepare, 我们把连续的、同目的地址、同段大小的包攒进一个
 * 缓冲, 用 sendmsg + UDP_SEGMENT 一次 syscall 发出(内核分段)。段大小需统一(除末段可更小)。
 * GSO 不支持(老内核/EIO)时逐段 sendto 回退, 保证正确性。 */
static void udp_send_batch(void){
    uint8_t gbuf[64*1536]; size_t glen=0; int gseg=0,gfd=-1; struct sockaddr_storage gto; socklen_t gtolen=0;
    for(;;){
        uint8_t sb[1536]; size_t sl=0; struct sockaddr_storage to,fr; int ifx=0;
        picoquic_connection_id_t lc; picoquic_cnx_t* lcnx=NULL;
        uint64_t send_now=picoquic_current_time();
        int r=picoquic_prepare_next_packet(G.quic,send_now,sb,sizeof(sb),&sl,&to,&fr,&ifx,&lc,&lcnx);
        if(r!=0||sl==0) break;
        int fd=(G.role==ROLE_MIDDLE&&pool_contains_cnx(lcnx))?G.client_udp_fd:G.udp_fd;
        socklen_t tl=(to.ss_family==AF_INET)?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
        if(!g_udp_gso_enabled){
            if(sendto(fd,sb,sl,0,(struct sockaddr*)&to,tl)<0){
                nb_metrics_note_udp_error(&g_metrics_state,1);
                if(send_now-nb_current_instance->udp_send_last_warn_at>=1000000ULL){
                    nb_current_instance->udp_send_last_warn_at=send_now;
                    log4c_warn("udp sendto failed: %s",strerror(errno));
                }
            }
            continue;
        }
        if(glen==0){ memcpy(&gto,&to,tl); gtolen=tl; gseg=(int)sl;gfd=fd; memcpy(gbuf,sb,sl); glen=sl; }
        else if(fd==gfd&&nb_udp_endpoint_equal(&to,&gto) && (int)sl<=gseg && glen+sl<=sizeof(gbuf)){
            memcpy(gbuf+glen,sb,sl); glen+=sl;
            if((int)sl<gseg){ size_t errors=nb_udp_send_gso(gfd,gbuf,glen,(size_t)gseg,(struct sockaddr*)&gto,gtolen);
                if(errors)nb_metrics_note_udp_error(&g_metrics_state,errors);
                glen=0;gseg=0;} /* 末段更小 -> flush */
        } else { /* 目的/段大小变化或缓冲满 -> flush 后新起一批 */
            size_t errors=nb_udp_send_gso(gfd,gbuf,glen,(size_t)gseg,(struct sockaddr*)&gto,gtolen);
            if(errors)nb_metrics_note_udp_error(&g_metrics_state,errors);
            memcpy(&gto,&to,tl); gtolen=tl; gseg=(int)sl;gfd=fd; memcpy(gbuf,sb,sl); glen=sl;
        }
    }
    if(glen>0){size_t errors=nb_udp_send_gso(gfd,gbuf,glen,(size_t)gseg,(struct sockaddr*)&gto,gtolen);
        if(errors)nb_metrics_note_udp_error(&g_metrics_state,errors);}
}

static void usage(const char* prog){
    fprintf(stderr,
        "Usage:\n"
        "  entry(固定target): %s -r entry -l <tcp_port> -n <hop1_host> -N <hop1_qport> -R <route> -c <cert> -k <key> -a <ca>\n"
        "  entry(SOCKS5入口): %s -r entry -l <socks_port> -n <hop1_host> -N <hop1_qport> -S -U <users> -Q <tenants> [-M <mid_route>] -c <cert> -k <key> -a <ca>\n"
        "  middle           : %s -r middle -p <quic_port> -c <cert> -k <key> -a <ca>\n"
        "  exit             : %s -r exit   -p <quic_port> -c <cert> -k <key> -a <ca>\n"
        "  route(发给第一跳): \"H:hop2:qport,...,T:target:port\" (两跳仅 \"T:target:port\")\n"
        "  -S: SOCKS5 入口, target 由 SOCKS5 CONNECT 动态获取; -M: 中间跳前缀如 \"H:kz:4443\"(空=两跳)\n"
        "  -F: TikTok 内部分流规则文件(默认尝试 /root/nb/tiktok_flow_rules.conf 或环境变量 NB_TIKTOK_RULES)\n",
        prog,prog,prog,prog);
}
