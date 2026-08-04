static proxy_stream_t* udp_exit_open(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view,const char* route){
    char host[256];int port=0;if(parse_target_from_route(route,host,sizeof(host),&port)!=0)return NULL;
    if(!whitelist_allowed(host,port))return NULL;
    proxy_stream_t* p=ps_alloc();if(p==NULL)return NULL;
    p->udp_mode=1;p->udp_session_id=view->session_id;p->up_cnx=cnx;
    if(udp_reassembly_ensure(p)!=0){ps_set_close_reason(p,"udp-exit-state-allocation");ps_free(p);return NULL;}
    ps_index_refresh(p);
    snprintf(p->route,sizeof(p->route),"%s",route);snprintf(p->udp_target_host,sizeof(p->udp_target_host),"%s",host);
    p->udp_target_port=port;p->target_port=port;nb_flow_policy_t pol;flow_policy_for_host(host,port,&pol);apply_flow_policy(p,&pol);
    if(dns_submit(p->id,host,port)!=0){ps_set_close_reason(p,"udp-dns-queue-full");ps_free(p);return NULL;}
    p->dns_pending=1;log4c_info("exit udp flow open id=%u usid=%u target=%s:%d",p->id,p->udp_session_id,host,port);return p;
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
        }else if(dgramq_append(&p->udp_pending_tx,&p->udp_pending_tx_len,&p->udp_pending_tx_cap,
            complete->payload,complete->payload_length,p->id)!=0)return -1;
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

static int udp_on_quic_datagram(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view){
    char route[NB_UDP_ROUTE_MAX];if(view->route_length>=sizeof(route))return -2;
    memcpy(route,view->route,view->route_length);route[view->route_length]=0;
    if(view->type==NB_UDP_TYPE_CLOSE_ACK){(void)nb_udp_lifecycle_ack(&g_udp_lifecycle,(uintptr_t)cnx,view->session_id);return 0;}
    if(view->type==NB_UDP_TYPE_CLOSE){
        proxy_stream_t* p=NULL;(void)udp_lifecycle_schedule(cnx,NB_UDP_TYPE_CLOSE_ACK,view->session_id,route);
        if(G.role==ROLE_ENTRY)p=ps_find_udp_down(cnx,view->session_id);else if(G.role==ROLE_EXIT)p=ps_find_udp_up(cnx,view->session_id);
        else {p=ps_find_udp_up(cnx,view->session_id);if(p==NULL)p=ps_find_udp_down(cnx,view->session_id);}
        if(p==NULL)return 0;
        picoquic_cnx_t* opposite=p->up_cnx==cnx?p->down_cnx:p->up_cnx;
        if(opposite!=NULL)(void)udp_lifecycle_schedule(opposite,NB_UDP_TYPE_CLOSE,p->udp_session_id,p->route);
        p->udp_close_received=1;ps_teardown_reason(p,"udp-peer-close");return 0;
    }
    if(view->type==NB_UDP_TYPE_C2S){
        if(G.role==ROLE_MIDDLE){
            proxy_stream_t* p=ps_find_udp_up(cnx,view->session_id);char rest[NB_UDP_ROUTE_MAX];
            if(p==NULL)p=udp_middle_open(cnx,view,route,rest,sizeof(rest));
            else snprintf(rest,sizeof(rest),"%s",p->route);
            if(p!=NULL&&p->udp_first_c2s_rx_at==0){p->udp_first_c2s_rx_at=picoquic_current_time();
                log4c_info("middle udp trace id=%u usid=%u stage=c2s-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            if(p==NULL)return -3;
            if(udp_forward_fragment(p,1,view,rest)!=0)return -4;
            p->udp_packets_c2s+=(view->fragment_index==0);p->bytes_c2s+=view->payload_length;ps_touch(p);return 0;
        }
        if(G.role==ROLE_EXIT){
            proxy_stream_t* p=ps_find_udp_up(cnx,view->session_id);if(p==NULL)p=udp_exit_open(cnx,view,route);
            if(p==NULL)return -5;
            if(p->udp_first_c2s_rx_at==0){p->udp_first_c2s_rx_at=picoquic_current_time();
                log4c_info("exit udp trace id=%u usid=%u stage=c2s-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            nb_udp_reassembled_t complete;
            if(p->udp_reassembly==NULL)return -6;
            int rc=nb_udp_reassembly_feed(p->udp_reassembly,view,picoquic_current_time(),&complete);
            return rc<0?-6:(rc==1?(udp_deliver_reassembled(p,&complete)==0?0:-7):0);
        }
    }else if(view->type==NB_UDP_TYPE_S2C){
        if(G.role==ROLE_MIDDLE){
            proxy_stream_t* p=ps_find_udp_down(cnx,view->session_id);
            if(p!=NULL&&p->udp_first_s2c_rx_at==0){p->udp_first_s2c_rx_at=picoquic_current_time();
                log4c_info("middle udp trace id=%u usid=%u stage=s2c-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_s2c_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            if(p==NULL)return -8;
            if(udp_forward_fragment(p,0,view,p->route)!=0)return -9;
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
                log4c_info("exit internal probe start id=%u mode=echo",p->id);
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
        { const uint8_t* control=NULL;size_t control_len=0,token=0;
          if(nb_udp_lifecycle_next(&g_udp_lifecycle,(uintptr_t)cnx,now,&control,&control_len,&token)){
            if(control_len<=length){uint8_t* dst=picoquic_provide_datagram_buffer_ex(bytes,control_len,picoquic_datagram_not_active);if(dst!=NULL){memcpy(dst,control,control_len);nb_udp_lifecycle_sent(&g_udp_lifecycle,token,now);handled=1;}}
            else {(void)picoquic_provide_datagram_buffer_ex(bytes,0,picoquic_datagram_not_active);handled=1;} } }
        for(int media_pass=0;media_pass<2&&!handled;media_pass++){
          for(int scan=0;scan<MAX_CONN&&!handled;scan++){
            int i=(int)((*datagram_rr+(uint32_t)scan)%MAX_CONN);
            p=&G.streams[i];if(!p->in_use||!p->udp_mode||p->udp_association)continue;
            if(media_pass==0&&p->flow_class!=NB_FLOW_CLASS_MEDIA)continue;
            if(media_pass==1&&p->flow_class==NB_FLOW_CLASS_MEDIA)continue;
            uint8_t* queue=NULL;size_t* queue_len=NULL;
            if(p->down_cnx==cnx&&p->udp_down_tx_len>=10){queue=p->udp_down_tx;queue_len=&p->udp_down_tx_len;}
            else if(p->up_cnx==cnx&&p->udp_up_tx_len>=10){queue=p->udp_up_tx;queue_len=&p->udp_up_tx_len;}
            if(queue==NULL)continue;
            nb_live_queue_limits_t limits=nb_live_queue_limits(p->flow_class,1);
            uint64_t dropped=dgramq_drop_expired(queue,queue_len,limits.deadline_us,now);
            if(dropped>0)log4c_info("%s udp deadline drop id=%u packets=%llu deadline=%.1fms target=%s:%d",
                role_name(G.role),p->id,(unsigned long long)dropped,limits.deadline_us/1000.0,
                p->udp_target_host,p->udp_target_port);
            const uint8_t* dg=NULL;size_t dg_len=0;uint64_t queued_at=0;
            if(!dgramq_peek(queue,*queue_len,&dg,&dg_len,&queued_at))continue;
            if(dg_len>length){
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
                    size_t taken=nb_tenant_take(&g_tenants,p->tenant_index,tenant_charge,now,tenant_direction);
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
        if(length>=24&&nb_get_u32(bytes)==NB_UDP_MAGIC){
            if(ev==picoquic_callback_datagram){
                nb_udp_wire_view_t view;
                int decode_rc=nb_udp_wire_decode(bytes,length,&view);
                if(decode_rc!=0)log4c_warn("%s udp datagram reject stage=decode length=%zu",
                    role_name(G.role),length);
                else {
                    int handler_rc=udp_on_quic_datagram(cnx,&view);
                    if(handler_rc!=0)log4c_warn("%s udp datagram reject stage=handler rc=%d length=%zu type=%u usid=%u seq=%u frag=%u/%u route_len=%u",
                        role_name(G.role),handler_rc,length,view.type,view.session_id,view.sequence,
                        view.fragment_index+1,view.fragment_count,view.route_length);
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
