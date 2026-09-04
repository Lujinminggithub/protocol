static int nb_runtime_id_ok(const char* value,size_t max_len){
    size_t i,n;if(value==NULL)return 0;n=strlen(value);if(n==0||n>max_len)return 0;
    for(i=0;i<n;i++){unsigned char c=(unsigned char)value[i];
        if(!(isalnum(c)||c=='_'||c=='-'||c=='.'))return 0;}
    return 1;
}

static void exit_recovery_fail_stream(proxy_stream_t* p,const char* reason){
    if(p==NULL||!p->in_use)return;
    if(p->tcp_fd>=0){
        if(G.epoll_fd>=0)epoll_ctl(G.epoll_fd,EPOLL_CTL_DEL,p->tcp_fd,NULL);
        close(p->tcp_fd);p->tcp_fd=-1;
    }
    if(p->up_cnx)picoquic_reset_stream(p->up_cnx,p->up_stream_id,1);
    ps_set_close_reason(p,reason);ps_free(p);
}

static int64_t exit_connect_wait_us(uint64_t now_us,int64_t wait_us){
    if(G.role!=ROLE_EXIT)return wait_us;
    for(int i=0;i<MAX_CONN;i++){
        proxy_stream_t* p=&G.streams[i];
        wait_us=ps_exit_connect_wait_us(p,now_us,wait_us);
    }
    return wait_us;
}

static void exit_connect_expire_due(uint64_t now_us){
    if(G.role!=ROLE_EXIT)return;
    for(int i=0;i<MAX_CONN;i++){
        proxy_stream_t* p=&G.streams[i];
        if(ps_exit_connect_timeout(p,now_us)!=NB_EXIT_RECOVERY_CONNECT_TIMEOUT)continue;
        uint64_t latency_us=ps_exit_connect_latency_us(p);
        nb_metrics_note_target_connect(&g_metrics_state,1,latency_us);
        log4c_warn("Exit目标建连超时 id=%u connect_ms=%.1f result=timeout route=%s",
            p->id,latency_us/1000.0,p->route);
        exit_recovery_fail_stream(p,"target-connect-timeout");
    }
}

static int nb_node_instance_loop(int argc,char**argv,nb_instance_t* instance){
    if(instance==NULL)return 1;
    nb_current_instance=instance;
    int i; int listen_port=0, quic_port=0, next_port=0;
    const char* rolestr=NULL; const char* next_host=NULL;
    const char* cert=NULL; const char* key=NULL; const char* cert_root=NULL; const char* route=NULL;
    const char* users_path=NULL;
    const char* tenants_path=NULL;
    const char* control_path_arg=NULL;
    const char* exit_routes_path=NULL;
    const char* outbound_ip=NULL;
    char control_path[108];
    const char* wl_path=NULL;
    const char* tiktok_rules_path=NULL;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"-r")&&i+1<argc) rolestr=argv[++i];
        else if(!strcmp(argv[i],"-l")&&i+1<argc) listen_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-p")&&i+1<argc) quic_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-n")&&i+1<argc) next_host=argv[++i];
        else if(!strcmp(argv[i],"-N")&&i+1<argc) next_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-R")&&i+1<argc) route=argv[++i];
        else if(!strcmp(argv[i],"-S")) G.socks_enabled=1;
        else if(!strcmp(argv[i],"-W")&&i+1<argc) wl_path=argv[++i];
        else if(!strcmp(argv[i],"-F")&&i+1<argc) tiktok_rules_path=argv[++i];
        else if(!strcmp(argv[i],"-M")&&i+1<argc) snprintf(G.mid_route,sizeof(G.mid_route),"%s",argv[++i]);
        else if(!strcmp(argv[i],"-c")&&i+1<argc) cert=argv[++i];
        else if(!strcmp(argv[i],"-k")&&i+1<argc) key=argv[++i];
        else if(!strcmp(argv[i],"-a")&&i+1<argc) cert_root=argv[++i];
        else if(!strcmp(argv[i],"-U")&&i+1<argc) users_path=argv[++i];
        else if(!strcmp(argv[i],"-Q")&&i+1<argc) tenants_path=argv[++i];
        else if(!strcmp(argv[i],"-C")&&i+1<argc) control_path_arg=argv[++i];
        else if(!strcmp(argv[i],"-E")&&i+1<argc) exit_routes_path=argv[++i];
        else if(!strcmp(argv[i],"-o")&&i+1<argc) outbound_ip=argv[++i];
        else if(!strcmp(argv[i],"--max-sessions")&&i+1<argc){
            unsigned long value=strtoul(argv[++i],NULL,10);if(value<1||value>MAX_CONN){fprintf(stderr,"invalid max sessions\n");return 1;}
            instance->session_limit=(uint32_t)value;
        }
        else if(!strcmp(argv[i],"--max-queue-bytes")&&i+1<argc){
            unsigned long long value=strtoull(argv[++i],NULL,10);if(value<1048576ULL||value>1073741824ULL){fprintf(stderr,"invalid max queue bytes\n");return 1;}
            instance->queue_limit_bytes=(uint64_t)value;
        }
    }
    if(rolestr==NULL){ usage(argv[0]); return 1; }
    if(!strcmp(rolestr,"entry")) G.role=ROLE_ENTRY;
    else if(!strcmp(rolestr,"middle")) G.role=ROLE_MIDDLE;
    else if(!strcmp(rolestr,"exit")) G.role=ROLE_EXIT;
    else { usage(argv[0]); return 1; }
    { const char* worker=nb_instance_env(nb_current_instance,"NB_WORKER_ID");if(!worker)worker="0";const char* dir=nb_instance_env(nb_current_instance,"NB_CONTROL_DIR");if(!dir||!dir[0])dir="/run";
      const char* prefix=nb_instance_env(nb_current_instance,"NB_CONTROL_PREFIX");char fallback[48];
      if(!prefix||!prefix[0]){snprintf(fallback,sizeof(fallback),"nb-%s",role_name(G.role));prefix=fallback;}
      if(!nb_runtime_id_ok(prefix,64)||!nb_runtime_id_ok(worker,16)){fprintf(stderr,"invalid control socket identity\n");return 1;}
      int n=control_path_arg?snprintf(control_path,sizeof(control_path),"%s",control_path_arg)
          :snprintf(control_path,sizeof(control_path),"%s/%s-%s.ctl",dir,prefix,worker);
      if(n<0||(size_t)n>=sizeof(control_path)){fprintf(stderr,"control socket path too long\n");return 1;}
      memcpy(instance->control_path,control_path,(size_t)n+1); }

    { char pname[96];const char* instance_name=nb_instance_env(nb_current_instance,"NB_INSTANCE_ID");
      if(instance_name&&instance_name[0]&&!nb_runtime_id_ok(instance_name,64)){fprintf(stderr,"invalid instance identity\n");return 1;}
      if(instance_name&&instance_name[0])snprintf(pname,sizeof(pname),"nb-%s-%s",instance_name,role_name(G.role));
      else snprintf(pname,sizeof(pname),"nb-%s",role_name(G.role));
      if(!instance->managed)log4c_init(pname);
      /* 生产默认 INFO(高频 per-stream 日志已降为 DEBUG, 不输出), 避免海量流日志拖慢单线程事件循环。
       * 需调试时 NB_LOG_LEVEL=DEBUG。 */
      const char* lv=nb_instance_env(nb_current_instance,"NB_LOG_LEVEL"); int lvl=LOG4C_INFO;
      if(lv){ if(!strcmp(lv,"DEBUG"))lvl=LOG4C_DEBUG; else if(!strcmp(lv,"WARN"))lvl=LOG4C_WARN; else if(!strcmp(lv,"ERROR"))lvl=LOG4C_ERROR; }
      log4c_set_level(lvl); }
    g_run_id=picoquic_current_time()^((uint64_t)(unsigned)getpid()<<32);
    if(!instance->managed)nb_runtime_init();
    log4c_info("runtime identity run=%llx worker=%s pid=%d",
        (unsigned long long)g_run_id,nb_instance_env(nb_current_instance,"NB_WORKER_ID")?nb_instance_env(nb_current_instance,"NB_WORKER_ID"):"0",(int)getpid());

    { const char* ug=nb_instance_env(nb_current_instance,"NB_UDP_GSO");
      if(ug&&(!strcmp(ug,"off")||!strcmp(ug,"0")))g_udp_gso_enabled=0;
      log4c_info("UDP GSO aggregation: %s",g_udp_gso_enabled?"enabled":"disabled"); }
    { const char* rg=nb_instance_env(nb_current_instance,"NB_REORDER_GAP"); const char* rd=nb_instance_env(nb_current_instance,"NB_REORDER_DELAY_US");
      if(rg&&rg[0])g_reorder_gap=strtoull(rg,NULL,10);
      if(rd&&rd[0])g_reorder_delay_us=strtoull(rd,NULL,10);
      if(G.role==ROLE_ENTRY || G.role==ROLE_MIDDLE)log4c_info("%s loss reorder tolerance: gap=%llu delay=%lluus (PTO unchanged)",
          role_name(G.role),(unsigned long long)g_reorder_gap,(unsigned long long)g_reorder_delay_us); }
    if(G.role==ROLE_ENTRY||G.role==ROLE_MIDDLE){
      g_pool_health_cfg.queue_age_threshold_us=env_ms_to_us("NB_POOL_RETIRE_QUEUE_MS",750,200,5000);g_pool_health_cfg.hold_us=env_ms_to_us("NB_POOL_RETIRE_HOLD_MS",5000,1000,60000);
      g_pool_health_cfg.progress_grace_us=env_ms_to_us("NB_POOL_RETIRE_STALL_MS",12000,5000,60000);
      g_pool_health_cfg.cooldown_us=env_ms_to_us("NB_POOL_RETIRE_COOLDOWN_MS",30000,5000,300000);
      log4c_info("pool recovery: queue=%.1fms hold=%.1fs stall=%.1fs cooldown=%.1fs",
          g_pool_health_cfg.queue_age_threshold_us/1000.0,g_pool_health_cfg.hold_us/1000000.0,
          g_pool_health_cfg.progress_grace_us/1000000.0,g_pool_health_cfg.cooldown_us/1000000.0);
    }
    if(G.role==ROLE_ENTRY)g_udp_control_grace_us=env_ms_to_us("NB_UDP_CONTROL_GRACE_MS",120000,100,120000);

    { const char* fe=nb_instance_env(nb_current_instance,"NB_FEC");
      if(fe&&fe[0])log4c_warn("NB_FEC is retired and ignored; use NB_FEC_V15_ACTIVE only for controlled tests"); }
    { const char* fv=nb_instance_env(nb_current_instance,"NB_FEC_V15");
      if(fv && (!strcmp(fv,"off")||!strcmp(fv,"0"))){ g_fec_v15_observe=0; }
      if(fv && (!strcmp(fv,"on")||!strcmp(fv,"1"))){
          log4c_warn("NB_FEC_V15=%s now means observe-only; use NB_FEC_V15_ACTIVE=on only in controlled tests", fv);
      }
      const char* fa=nb_instance_env(nb_current_instance,"NB_FEC_V15_ACTIVE");
      if(fa && (!strcmp(fa,"on")||!strcmp(fa,"1"))){
          g_fec_v15_active=1;
          g_fec_v15_observe=1;
      }
      log4c_info("FEC v1.5 mode: observe=%d active=%d", g_fec_v15_observe, g_fec_v15_active); }
    { const char* ff=nb_instance_env(nb_current_instance,"NB_FEC_V15_FORCE"); /* V1.5 测试强开: 无需 line_bad 即开启 sidecar */
      if(ff && (!strcmp(ff,"on")||!strcmp(ff,"1"))){ g_fec_v15_force=1; log4c_info("FEC v1.5 sidecar force mode: ENABLED (NB_FEC_V15_FORCE=%s)",ff); } }
    { const char* fr=nb_instance_env(nb_current_instance,"NB_FEC_V15_FORCE_ROBUST");
      if(fr && (!strcmp(fr,"on")||!strcmp(fr,"1"))){ g_fec_v15_force_robust=1; log4c_info("FEC robust profile override: ENABLED"); } }
    { const char* ds=nb_instance_env(nb_current_instance,"NB_FEC_V15_DROP_SRC_MOD");
      if(ds && atoi(ds)>0){ g_fec_v15_drop_src_mod=(uint32_t)atoi(ds); log4c_info("FEC v1.5 test drop source mod=%u", g_fec_v15_drop_src_mod); } }
    { const char* dr=nb_instance_env(nb_current_instance,"NB_FEC_V15_DROP_REPAIR_MOD");
      if(dr && atoi(dr)>0){ g_fec_v15_drop_repair_mod=(uint32_t)atoi(dr); log4c_info("FEC v1.5 test drop repair mod=%u", g_fec_v15_drop_repair_mod); } }
    {
      const char* eb=nb_instance_env(nb_current_instance,"NB_FEC_BDP_BITRATE_BPS"); if(eb&&strtoull(eb,NULL,10)>0) g_fec_bdp_bitrate_bps=strtoull(eb,NULL,10);
      const char* er=nb_instance_env(nb_current_instance,"NB_FEC_BDP_RTT_US"); if(er&&strtoull(er,NULL,10)>0) g_fec_bdp_rtt_us=strtoull(er,NULL,10);
      const char* em=nb_instance_env(nb_current_instance,"NB_FEC_BDP_MULTIPLIER"); if(em&&atoi(em)>0) g_fec_bdp_multiplier=(unsigned)atoi(em);
      if(nb_fec_config_for_bdp(&g_fec_cfg,NB_FEC_DEFAULT_K,NB_FEC_DEFAULT_R,
          g_fec_bdp_bitrate_bps,g_fec_bdp_rtt_us,g_fec_bdp_multiplier)!=NB_FEC_OK){
          log4c_error("FEC config invalid"); log4c_shutdown(); return 1;
      }
      log4c_info("FEC engine cfg k=%u r=%u history=%u blocks rx_window=%u blocks hold=%lluus nack=%lluus retry=%lluus/%u",
          g_fec_cfg.k,g_fec_cfg.r,g_fec_cfg.tx_history_blocks,g_fec_cfg.rx_window_blocks,
          (unsigned long long)g_fec_cfg.block_hold_us,(unsigned long long)g_fec_cfg.nack_delay_us,
          (unsigned long long)g_fec_cfg.nack_retry_us,g_fec_cfg.max_nack_retries);
    }

    { char whitelist_error[256];instance->whitelist=nb_whitelist_create(wl_path,whitelist_error,sizeof(whitelist_error));if(instance->whitelist==NULL){
        log4c_error("whitelist requested but could not be loaded: %s; refusing to start",whitelist_error);
        log4c_shutdown();return 1;
    } }
    instance->policy=nb_policy_create(tiktok_rules_path ? tiktok_rules_path : nb_instance_env(nb_current_instance,"NB_TIKTOK_RULES"));
    if(instance->policy==NULL){log4c_error("flow policy allocation failed");log4c_shutdown();return 1;}

    int insecure_test=0;
    { const char* value=nb_instance_env(nb_current_instance,"NB_INSECURE_TEST_MODE");insecure_test=value&&(!strcmp(value,"1")||!strcmp(value,"on")); }
    if(!insecure_test&&(!cert||!key||!cert_root)){
        log4c_error("secure mode requires -c node certificate, -k private key and -a CA certificate");
        log4c_shutdown();return 1;
    }
    if(!insecure_test&&((G.role==ROLE_ENTRY&&G.socks_enabled)||G.role==ROLE_EXIT)&&!wl_path){
        log4c_error("secure SOCKS entry and exit require fail-closed -W <whitelist>");
        log4c_shutdown();return 1;
    }
    if(insecure_test)log4c_warn("NB_INSECURE_TEST_MODE enabled: node certificate authentication is not enforced");
    if(key){
        char error[256];
        if(nb_auth_check_private_file(key,error,sizeof(error))!=0){
            log4c_error("private key rejected: %s",error);log4c_shutdown();return 1;
        }
    }
    if(tenants_path&&(G.role==ROLE_ENTRY||G.role==ROLE_EXIT)){char error[256],state_path[512];const char* configured_state=nb_instance_env(nb_current_instance,"NB_TENANT_STATE");
        if(nb_tenants_load(&g_tenants,tenants_path,error,sizeof(error))!=0){log4c_error("tenant config rejected: %s",error);log4c_shutdown();return 1;}
        if(configured_state&&*configured_state)snprintf(state_path,sizeof(state_path),"%s",configured_state);
        else if(snprintf(state_path,sizeof(state_path),"%s.state",tenants_path)<0||strlen(state_path)>=sizeof(state_path)-1){log4c_error("tenant state path is too long");log4c_shutdown();return 1;}
        if(nb_tenants_enable_shared(&g_tenants,state_path,error,sizeof(error))!=0){log4c_error("shared tenant state rejected: %s",error);log4c_shutdown();return 1;}
        g_tenants_enabled=1;nb_current_instance->tenant_generation=g_tenants.config_fingerprint;
        log4c_info("tenant quotas loaded role=%s tenants=%zu generation=%llu shared_state=%s",
            role_name(G.role),g_tenants.count,(unsigned long long)nb_current_instance->tenant_generation,state_path);
    }
    if(G.role==ROLE_ENTRY&&G.socks_enabled){
        const char* udp_advertise=nb_instance_env(nb_current_instance,"NB_SOCKS_UDP_ADVERTISE_IP");
        if(udp_advertise&&udp_advertise[0]){
            if(inet_pton(AF_INET,udp_advertise,&g_socks_udp_advertise_addr)!=1){
                log4c_error("invalid NB_SOCKS_UDP_ADVERTISE_IP=%s",udp_advertise);log4c_shutdown();return 1;
            }
            g_socks_udp_advertise_configured=1;
            log4c_info("SOCKS UDP advertise address: %s",udp_advertise);
        }else log4c_info("SOCKS UDP advertise address: accepted TCP local address");
        const char* udp_port_min=nb_instance_env(nb_current_instance,"NB_SOCKS_UDP_PORT_MIN");
        const char* udp_port_max=nb_instance_env(nb_current_instance,"NB_SOCKS_UDP_PORT_MAX");
        if((udp_port_min&&udp_port_min[0])||(udp_port_max&&udp_port_max[0])){
            char* end_min=NULL;char* end_max=NULL;unsigned long min_port=udp_port_min?strtoul(udp_port_min,&end_min,10):0;
            unsigned long max_port=udp_port_max?strtoul(udp_port_max,&end_max,10):0;
            if(!udp_port_min||!udp_port_max||end_min==udp_port_min||end_max==udp_port_max||*end_min||*end_max||
                min_port<1024||max_port>65535||min_port>max_port||max_port-min_port+1>16384){
                log4c_error("invalid SOCKS UDP port range min=%s max=%s",
                    udp_port_min?udp_port_min:"(unset)",udp_port_max?udp_port_max:"(unset)");log4c_shutdown();return 1;
            }
            g_socks_udp_port_min=(uint16_t)min_port;g_socks_udp_port_max=(uint16_t)max_port;
            g_socks_udp_port_next=g_socks_udp_port_min;
            log4c_info("SOCKS UDP relay port range: %u-%u capacity=%u",g_socks_udp_port_min,
                g_socks_udp_port_max,(unsigned)(g_socks_udp_port_max-g_socks_udp_port_min+1));
        }else log4c_info("SOCKS UDP relay port range: ephemeral");
        if(!users_path&&!insecure_test){log4c_error("secure SOCKS mode requires -U <users file>");log4c_shutdown();return 1;}
        if(users_path){
            char error[256];
            if(nb_auth_users_load(&g_socks_users,users_path,error,sizeof(error))!=0){
                log4c_error("SOCKS users rejected: %s",error);log4c_shutdown();return 1;
            }
            g_socks_auth_enabled=1;log4c_info("SOCKS authentication loaded users=%zu",g_socks_users.count);
        }
        if(exit_routes_path){
            char error[256];
            if(nb_routes_load(&G.exit_routes,exit_routes_path,error,sizeof(error))!=0){
                log4c_error("exit routes rejected: %s",error);log4c_shutdown();return 1;
            }
            G.exit_routes_enabled=1;log4c_info("exit routes loaded count=%zu",G.exit_routes.count);
        }
    }

    if(G.role==ROLE_ENTRY){
        int is_name=0;
        if(!listen_port||!next_host||!next_port||(!G.socks_enabled && !route)){ usage(argv[0]); log4c_shutdown(); return 1; }
        next_port=worker_lane_port(next_port);if(next_port<1){log4c_error("invalid worker lane next-hop port");log4c_shutdown();return 1;}
        if(picoquic_get_server_address(next_host,next_port,&G.next_addr,&is_name)!=0){
            log4c_error("resolve hop1 %s:%d fail",next_host,next_port); log4c_shutdown(); return 1; }
        G.next_configured=1;
        if(!G.socks_enabled){
            size_t route_len=strlen(route);
            if(route_len==0||route_len>=sizeof(G.route_str)){log4c_error("fixed route exceeds limit");log4c_shutdown();return 1;}
            memcpy(G.route_str,route,route_len+1);
        }
    } else {
        if(!quic_port){ usage(argv[0]); log4c_shutdown(); return 1; }
        quic_port=worker_lane_port(quic_port);if(quic_port<1){log4c_error("invalid worker lane listen port");log4c_shutdown();return 1;}
    }
    if(outbound_ip){
        if(G.role!=ROLE_EXIT){log4c_error("-o outbound source address is only valid for exit");log4c_shutdown();return 1;}
        struct sockaddr_in* source=(struct sockaddr_in*)&G.outbound_addr;
        memset(&G.outbound_addr,0,sizeof(G.outbound_addr));source->sin_family=AF_INET;
        if(inet_pton(AF_INET,outbound_ip,&source->sin_addr)!=1){log4c_error("invalid IPv4 outbound source: %s",outbound_ip);log4c_shutdown();return 1;}
        source->sin_port=0;G.outbound_configured=1;log4c_info("exit outbound source: %s",outbound_ip);
    }

    G.quic=picoquic_create(MAX_CONN, cert, key, cert_root, NB_ALPN,
        G.role==ROLE_ENTRY?NULL:relay_quic_callback, G.role==ROLE_ENTRY?NULL:&G, NULL,NULL,NULL,
        picoquic_current_time(), NULL,NULL,NULL,0);
    if(G.quic==NULL){ log4c_error("quic create fail"); log4c_shutdown(); return 1; }
    picoquic_set_default_pmtud_policy(G.quic,picoquic_pmtud_required);
    log4c_info("path MTU discovery: required");
    if(G.role!=ROLE_ENTRY){
        picoquic_set_cookie_mode(G.quic,2);
        if(!insecure_test)picoquic_set_client_authentication(G.quic,1);
    }
    { const char* cc=nb_instance_env(nb_current_instance,"NB_CC");
      const char* bbr_options=nb_instance_env(nb_current_instance,"NB_BBR_OPTIONS");
      if(cc&&strcmp(cc,"cubic")==0){
          picoquic_set_default_congestion_algorithm(G.quic,picoquic_cubic_algorithm);
          log4c_info("congestion control: cubic");
      }else if(cc&&strcmp(cc,"dcubic")==0){
          picoquic_set_default_congestion_algorithm(G.quic,picoquic_dcubic_algorithm);
          log4c_info("congestion control: dcubic");
      }else if(cc&&strcmp(cc,"fastcc")==0){
          picoquic_set_default_congestion_algorithm(G.quic,picoquic_fastcc_algorithm);
          log4c_info("congestion control: fastcc");
      }else{
          if(cc&&cc[0]&&strcmp(cc,"bbr")!=0)
              log4c_warn("unknown NB_CC=%s, fallback to bbr",cc);
          if(bbr_options&&bbr_options[0])
              picoquic_set_default_congestion_algorithm_ex(G.quic,picoquic_bbr_algorithm,bbr_options);
          else
              picoquic_set_default_congestion_algorithm(G.quic,picoquic_bbr_algorithm);
          log4c_info("congestion control: bbr options=%s",(bbr_options&&bbr_options[0])?bbr_options:"default");
      } }
    { const char* value=nb_instance_env(nb_current_instance,"NB_CWIN_MAX_BYTES");
      if(value&&value[0]){
          char* end=NULL; errno=0;
          unsigned long long limit=strtoull(value,&end,10);
          if(errno!=0||end==value||*end!='\0'||limit<65536ULL||limit>67108864ULL){
              log4c_error("invalid NB_CWIN_MAX_BYTES=%s (expected 65536..67108864)",value);
              log4c_shutdown(); return 1;
          }
          picoquic_set_cwin_max(G.quic,(uint64_t)limit);
          log4c_info("congestion window cap: %llu bytes (%lluKB)",limit,limit/1024ULL);
      }else{
          log4c_info("congestion window cap: unlimited");
      } }
    { const char* value=nb_instance_env(nb_current_instance,"NB_MTU_MAX");
      if(value&&value[0]){
          char* end=NULL;errno=0;unsigned long mtu=strtoul(value,&end,10);
          if(errno!=0||end==value||*end!='\0'||mtu<1280||mtu>PICOQUIC_MAX_PACKET_SIZE){
              log4c_error("invalid NB_MTU_MAX=%s (expected 1280..%d)",value,PICOQUIC_MAX_PACKET_SIZE);
              log4c_shutdown();return 1;
          }
          picoquic_set_mtu_max(G.quic,(uint32_t)mtu);
          log4c_info("path MTU cap: %lu bytes (IPv4 QUIC UDP payload <= %lu)",mtu,mtu-28);
      }else log4c_info("path MTU cap: picoquic discovery default"); }
    { const char* path=nb_instance_env(nb_current_instance,"NB_TRANSPORT_PROFILE_FILE");
      if(path&&path[0]&&access(path,F_OK)==0){
          char error[256]={0};nb_transport_profile_t profile;
          if(nb_transport_profile_load(path,&profile,error,sizeof(error))!=0){
              log4c_error("startup transport profile rejected: %s",error);log4c_shutdown();return 1;
          }
          const char* expected_line=nb_instance_env(nb_current_instance,"NB_TRANSPORT_LINE_ID");
          if(expected_line==NULL||strcmp(profile.line_id,expected_line)||strcmp(profile.role,role_name(G.role))){
              log4c_error("startup transport profile identity mismatch");log4c_shutdown();return 1;
          }
          nb_current_instance->transport_profiles.active=profile;
          nb_current_instance->transport_profiles.has_active=1;
          if(transport_apply_active_profile()!=0){
              log4c_error("startup transport profile apply failed");log4c_shutdown();return 1;
          }
      }else if(path&&path[0]&&errno!=ENOENT){
          log4c_error("startup transport profile unavailable: %s",strerror(errno));log4c_shutdown();return 1;
      } }
    /* 单流初始信用保持在 128 KiB。middle 对入站流启用应用流控，并按下游队列
     * 消费进度增发信用，避免自动扩窗把秒级媒体积压隐藏在桥接队列中。
     * connection 级 max_data 仍为 64 MiB，以支撑大量并发流。 */
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_streams_bidi, 2000);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_stream_data_bidi_local, NB_STREAM_FLOW_WINDOW);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_stream_data_bidi_remote, NB_STREAM_FLOW_WINDOW);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_data, 67108864);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_max_datagram_frame_size, PICOQUIC_MAX_PACKET_SIZE);
    /* 优化2: 降低 max_ack_delay(默认25ms)到 5ms -> 接收方更快回 ACK, 拥塞控制更灵敏、RTT 感知更低,
     * 对直播/实时流延迟直接有益(代价: ACK 包略增, 80Mbps 下开销可忽略)。 */
    picoquic_set_default_tp_value(G.quic, picoquic_tp_max_ack_delay, 5000);

    /* UDP socket: entry 用临时端口(client); middle/exit bind quic_port(server)。 */
    G.udp_fd=socket(AF_INET,SOCK_DGRAM,0);
    if(G.udp_fd<0||nb_udp_enable_rxq_overflow(G.udp_fd)!=0){
        log4c_error("udp SO_RXQ_OVFL enable failed: %s",strerror(errno));log4c_shutdown();return 1;
    }
    { int one=1;if(setsockopt(G.udp_fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one))!=0){
        log4c_error("udp SO_REUSEPORT failed: %s",strerror(errno));log4c_shutdown();return 1;
    } }
    if(configure_udp_socket_buffers(G.udp_fd,"server/main")!=0){log4c_shutdown();return 1;}
    { struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY;
      a.sin_port=htons(G.role==ROLE_ENTRY?0:(uint16_t)quic_port);
      if(bind(G.udp_fd,(struct sockaddr*)&a,sizeof(a))<0){ log4c_error("bind udp fail: %s",strerror(errno)); log4c_shutdown(); return 1; }
      set_nonblock(G.udp_fd); }
    { socklen_t ll=sizeof(G.local_addr); memset(&G.local_addr,0,sizeof(G.local_addr));
      getsockname(G.udp_fd,(struct sockaddr*)&G.local_addr,&ll);
      if(G.local_addr.ss_family==AF_INET){ struct sockaddr_in* s4=(struct sockaddr_in*)&G.local_addr;
        if(s4->sin_addr.s_addr==INADDR_ANY) s4->sin_addr.s_addr=htonl(INADDR_LOOPBACK); } }
    if(G.role==ROLE_MIDDLE){struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=INADDR_ANY;G.client_udp_fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(G.client_udp_fd<0||nb_udp_enable_rxq_overflow(G.client_udp_fd)!=0||configure_udp_socket_buffers(G.client_udp_fd,"middle/client")!=0||bind(G.client_udp_fd,(struct sockaddr*)&a,sizeof(a))<0){log4c_error("middle client udp bind failed: %s",strerror(errno));return 1;}set_nonblock(G.client_udp_fd);socklen_t ll=sizeof(G.client_local_addr);getsockname(G.client_udp_fd,(struct sockaddr*)&G.client_local_addr,&ll);}

    if(G.role==ROLE_ENTRY){
        G.tcp_listen_fd=socket(AF_INET,SOCK_STREAM,0);
        int one=1; setsockopt(G.tcp_listen_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        if(setsockopt(G.tcp_listen_fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one))!=0){
            log4c_error("tcp SO_REUSEPORT failed: %s",strerror(errno));log4c_shutdown();return 1;
        }
        struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons((uint16_t)listen_port);
        if(bind(G.tcp_listen_fd,(struct sockaddr*)&a,sizeof(a))<0){ log4c_error("bind tcp :%d fail: %s",listen_port,strerror(errno)); log4c_shutdown(); return 1; }
        listen(G.tcp_listen_fd,128); set_nonblock(G.tcp_listen_fd);
        if(entry_open_upstream()!=0){ log4c_error("entry open upstream fail"); log4c_shutdown(); return 1; }
        if(G.socks_enabled)
            log4c_info("entry up: SOCKS5 :%d -> QUIC %s:%d mid=[%s] cc=%s",listen_port,next_host,next_port,G.mid_route,
                (nb_instance_env(nb_current_instance,"NB_CC")&&nb_instance_env(nb_current_instance,"NB_CC")[0])?nb_instance_env(nb_current_instance,"NB_CC"):"bbr");
        else
            log4c_info("entry up: TCP :%d -> QUIC %s:%d route=%s cc=%s",listen_port,next_host,next_port,route,
                (nb_instance_env(nb_current_instance,"NB_CC")&&nb_instance_env(nb_current_instance,"NB_CC")[0])?nb_instance_env(nb_current_instance,"NB_CC"):"bbr");
    } else {
        log4c_info("%s up: QUIC :%d cc=%s",role_name(G.role),quic_port,
            (nb_instance_env(nb_current_instance,"NB_CC")&&nb_instance_env(nb_current_instance,"NB_CC")[0])?nb_instance_env(nb_current_instance,"NB_CC"):"bbr");
    }

    const char* dns_servers=G.role==ROLE_EXIT?nb_instance_env(nb_current_instance,"NB_DNS_SERVERS"):NULL;
    nb_current_instance->dns=nb_dns_create_with_servers(G.outbound_configured?G.outbound_addr.ss_family:AF_UNSPEC,DNS_WORKERS,dns_servers);
    if(nb_current_instance->dns==NULL){ log4c_error("async DNS init fail"); log4c_shutdown(); return 1; }
    log4c_info("async DNS: %d workers ready servers=%s",DNS_WORKERS,(dns_servers&&dns_servers[0])?dns_servers:"system");
    if(g_socks_auth_enabled&&(nb_current_instance->auth_async=nb_auth_async_create(&g_socks_users,AUTH_WORKERS))==NULL){
        log4c_error("async SOCKS authentication init fail");log4c_shutdown();return 1;
    }
    if(g_socks_auth_enabled)log4c_info("async SOCKS authentication: %d workers ready",AUTH_WORKERS);
    G.epoll_fd=epoll_create1(EPOLL_CLOEXEC);
    if(G.epoll_fd<0){log4c_error("epoll_create1 fail: %s",strerror(errno));log4c_shutdown();return 1;}
    { char error[256];G.control_fd=nb_control_open(control_path,error,sizeof(error));
      if(G.control_fd<0){log4c_error("control socket failed: %s",error);return 1;}
      log4c_info("control socket: %s",control_path); }
    {
        struct epoll_event ev;memset(&ev,0,sizeof(ev));ev.events=EPOLLIN|EPOLLERR;
        ev.data.u64=NB_EPOLL_TAG_UDP;
        if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,G.udp_fd,&ev)!=0){log4c_error("epoll add udp fail: %s",strerror(errno));return 1;}
        if(G.client_udp_fd>=0){ev.data.u64=NB_EPOLL_TAG_CLIENT_UDP;if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,G.client_udp_fd,&ev)!=0)return 1;}
        ev.data.u64=NB_EPOLL_TAG_DNS;
        if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,nb_dns_context_result_fd(nb_current_instance->dns),&ev)!=0){log4c_error("epoll add dns fail: %s",strerror(errno));return 1;}
        if(g_socks_auth_enabled){ev.data.u64=NB_EPOLL_TAG_AUTH;
            if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,nb_auth_async_context_result_fd(nb_current_instance->auth_async),&ev)!=0){log4c_error("epoll add auth fail: %s",strerror(errno));return 1;}}
        if(G.tcp_listen_fd>=0){ev.data.u64=NB_EPOLL_TAG_LISTEN;
            if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,G.tcp_listen_fd,&ev)!=0){log4c_error("epoll add listen fail: %s",strerror(errno));return 1;}}
        ev.data.u64=NB_EPOLL_TAG_CONTROL;
        if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,G.control_fd,&ev)!=0){log4c_error("epoll add control fail: %s",strerror(errno));return 1;}
    }
    log4c_info("event loop: epoll enabled");
    atomic_store(&instance->ready,1);atomic_store(&instance->running,1);

    /* ---- 自定义事件循环 ---- */
    uint64_t loop_stat_at=picoquic_current_time(),loop_busy_max=0,loop_wake_late_max=0;
    uint64_t loop_iterations=0,loop_over_5ms=0,loop_over_20ms=0;
    while(!atomic_load(&instance->stop_requested)){
        uint64_t now=picoquic_current_time();
        udp_lifecycle_wake_due(now);
        int64_t wd=picoquic_get_next_wake_delay(G.quic,now,1000000); /* us, cap 1s */
        if(g_tenants_enabled)for(i=0;i<MAX_CONN;i++){
            proxy_stream_t* p=&G.streams[i];if(!p->in_use||p->tcp_fd<0)continue;
            wd=nb_tenant_wake_delay(p->tenant_throttled_until,now,wd);
        }
        for(i=0;i<MAX_CONN;i++){
            proxy_stream_t* p=&G.streams[i];if(!p->in_use||p->fec_engine==NULL)continue;
            uint64_t dl=nb_fec_next_deadline(p->fec_engine);if(dl==0)continue;
            int64_t fwd=(dl<=now)?0:(int64_t)(dl-now);if(fwd<wd)wd=fwd;
        }
        for(i=0;i<MAX_CONN;i++){
            proxy_stream_t* p=&G.streams[i];if(!p->in_use||p->udp_fec_tx==NULL)continue;
            uint64_t dl=nb_udp_fec_tx_next_deadline(p->udp_fec_tx);if(dl==0)continue;
            int64_t fwd=(dl<=now)?0:(int64_t)(dl-now);if(fwd<wd)wd=fwd;
        }
        { uint64_t dl=nb_udp_lifecycle_next_deadline(&g_udp_lifecycle);if(dl){int64_t d=(dl<=now)?0:(int64_t)(dl-now);if(d<wd)wd=d;} }
        wd=exit_connect_wait_us(now,wd);
        int64_t wait_us=wd<=0?0:(wd>1000000?1000000:wd);
        uint64_t wait_started=now,iteration_wake_late=0;
        struct epoll_event events[256];
        int event_count=nb_epoll_wait_us(G.epoll_fd,events,
            (int)(sizeof(events)/sizeof(events[0])),wait_us);
        if(event_count<0&&errno!=EINTR){log4c_error("epoll_wait fail: %s",strerror(errno));break;}
        if(event_count<0)event_count=0;
        now=picoquic_current_time();
        uint64_t busy_started=now;
        if(event_count==0){
            uint64_t expected=(uint64_t)wait_us,elapsed=now-wait_started;
            uint64_t late=elapsed>expected?elapsed-expected:0;
            iteration_wake_late=late;
            if(late>loop_wake_late_max)loop_wake_late_max=late;
        }
        for(int ei=0;ei<event_count;ei++){
            uint64_t tag=events[ei].data.u64;uint32_t ee=events[ei].events;
            if(tag==NB_EPOLL_TAG_UDP){udp_drain_fd(G.udp_fd,&G.local_addr);continue;}
            if(tag==NB_EPOLL_TAG_CLIENT_UDP){udp_drain_fd(G.client_udp_fd,&G.client_local_addr);continue;}
            if(tag==NB_EPOLL_TAG_AUTH){
                char drain[256];while(read(nb_auth_async_context_result_fd(nb_current_instance->auth_async),drain,sizeof(drain))>0){}
                nb_auth_async_result_t result;
                while(nb_auth_async_context_pop(nb_current_instance->auth_async,&result)){
                    if(result.valid)nb_auth_cache_store(&g_socks_users,result.fingerprint,
                        picoquic_current_time(),SOCKS_AUTH_CACHE_TTL_US);
                    for(int i=0;i<MAX_CONN;i++){
                        proxy_stream_t* p=&G.streams[i];
                        if(!p->in_use||!p->socks_auth_pending||
                            memcmp(p->socks_auth_fingerprint,result.fingerprint,sizeof(result.fingerprint))!=0)continue;
                        p->socks_auth_pending=0;p->socks_auth_username[0]=0;
                        memset(p->socks_auth_fingerprint,0,sizeof(p->socks_auth_fingerprint));
                        if(socks_auth_finish(p,result.username,result.valid,0,result.work_us)&&p->socks_len>0)socks_handshake(p);
                    }
                    memset(&result,0,sizeof(result));
                }
                continue;
            }
            if(tag==NB_EPOLL_TAG_DNS){
                char drain[256];while(read(nb_dns_context_result_fd(nb_current_instance->dns),drain,sizeof(drain))>0){}
                dns_res_t r;
                while(dns_pop_result(&r)){
                    proxy_stream_t* p=ps_find_by_id(r.ps_id);
                    if(!p||!p->dns_pending)continue;
                    uint64_t dns_done_us=r.completed_at_us?r.completed_at_us:picoquic_current_time();
                    uint64_t dns_latency_us=dns_done_us>=r.submitted_at_us?
                        dns_done_us-r.submitted_at_us:0;
                    char dns_address[96]="-";
                    if(r.ok&&getnameinfo((struct sockaddr*)&r.addr,r.addrlen,
                        dns_address,sizeof(dns_address),NULL,0,NI_NUMERICHOST)!=0)
                        snprintf(dns_address,sizeof(dns_address),"unknown");
                    int private_rejected=r.ok&&
                        nb_dns_tiktok_private_answer(r.host,(const struct sockaddr*)&r.addr);
                    nb_exit_recovery_action_t dns_action=ps_exit_dns_result(p,r.ok,
                        private_rejected,dns_done_us);
                    if(dns_action==NB_EXIT_RECOVERY_NONE)continue;
                    const char* dns_result=dns_action==NB_EXIT_RECOVERY_DNS_FAIL?"fail":
                        (dns_action==NB_EXIT_RECOVERY_PRIVATE_REJECT?"private-unreachable":"ok");
                    nb_metrics_note_dns(&g_metrics_state,!r.ok,private_rejected,dns_latency_us);
                    log4c_info("Exit DNS完成 id=%u host=%s address=%s dns_ms=%.3f result=%s",
                        p->id,r.host[0]?r.host:"-",dns_address,dns_latency_us/1000.0,dns_result);
                    if(dns_action==NB_EXIT_RECOVERY_DNS_FAIL){
                        exit_recovery_fail_stream(p,"target-dns-fail");continue;
                    }
                    if(dns_action==NB_EXIT_RECOVERY_PRIVATE_REJECT){
                        exit_recovery_fail_stream(p,"target-private-unreachable");continue;
                    }
                    if(p->udp_mode&&!p->udp_association){
                        int ufd=udp_connect_addr((struct sockaddr*)&r.addr,r.addrlen);
                        if(ufd<0){log4c_warn("exit udp target connect fail id=%u error=%s",p->id,strerror(errno));
                            ps_teardown_reason(p,"udp-target-connect-fail");continue;}
                        p->udp_fd=ufd;
                        if(epoll_udp_update(p,1)!=0){ps_teardown_reason(p,"udp-target-epoll-fail");continue;}
                        p->udp_target_ready_at=picoquic_current_time();
                        log4c_info("exit udp trace id=%u usid=%u stage=target-ready age=%.1fms pending=%zu target=%s:%d",
                            p->id,p->udp_session_id,trace_delta_ms(p->udp_target_ready_at,p->created_at),
                            p->udp_pending_tx_len,p->udp_target_host,p->udp_target_port);
                        { uint64_t reorder_delay_us=nb_current_instance->transport_ingress.active?
                              nb_current_instance->transport_ingress.link.reorder_delay_us:
                              nb_current_instance->reorder_delay_us;
                          nb_live_queue_limits_t limits=nb_live_queue_limits_for_path(
                              p->flow_class,1,reorder_delay_us);
                          uint64_t dropped=nb_live_queue_expiry_enabled(p->flow_class,1)?
                              dgramq_drop_expired(p->udp_pending_tx,&p->udp_pending_tx_len,
                                  limits.deadline_us,picoquic_current_time()):0;
                          if(dropped>0)log4c_info("exit udp pending deadline drop id=%u packets=%llu",
                              p->id,(unsigned long long)dropped); }
                        while(p->udp_pending_tx_len>=10){
                            const uint8_t* pending=NULL;size_t pending_len=0;
                            if(!dgramq_peek(p->udp_pending_tx,p->udp_pending_tx_len,&pending,&pending_len,NULL))break;
                            if(send(p->udp_fd,pending,pending_len,MSG_NOSIGNAL)!=(ssize_t)pending_len){
                                ps_teardown_reason(p,"udp-target-send-fail");break;
                            }
                            if(p->udp_first_target_send_at==0){
                                p->udp_first_target_send_at=picoquic_current_time();
                                log4c_info("exit udp trace id=%u usid=%u stage=target-first-send age=%.1fms ready_to_send=%.1fms bytes=%zu target=%s:%d",
                                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_target_send_at,p->created_at),
                                    trace_delta_ms(p->udp_first_target_send_at,p->udp_target_ready_at),pending_len,
                                    p->udp_target_host,p->udp_target_port);
                            }
                            dgramq_consume(p->udp_pending_tx,&p->udp_pending_tx_len);
                        }
                        if(p->in_use)log4c_info("exit udp target ready id=%u usid=%u target=%s:%d",
                            p->id,p->udp_session_id,p->udp_target_host,p->udp_target_port);
                        continue;
                    }
                    ps_exit_connect_started(p,picoquic_current_time());
                    int fd=tcp_connect_addr((struct sockaddr*)&r.addr,r.addrlen);
                    if(fd<0){
                        int connect_error=errno;
                        (void)ps_exit_connect_complete(p,0,picoquic_current_time());
                        uint64_t latency_us=ps_exit_connect_latency_us(p);
                        nb_metrics_note_target_connect(&g_metrics_state,0,latency_us);
                        log4c_warn("Exit目标建连启动失败 id=%u connect_ms=%.1f result=start-fail error=%s route=%s",
                            p->id,latency_us/1000.0,strerror(connect_error),p->route);
                        exit_recovery_fail_stream(p,"target-connect-start-fail");continue;
                    }
                    p->tcp_fd=fd;
                    if(epoll_tcp_update(p,1)!=0){
                        int epoll_error=errno;
                        (void)ps_exit_connect_complete(p,0,picoquic_current_time());
                        uint64_t latency_us=ps_exit_connect_latency_us(p);
                        nb_metrics_note_target_connect(&g_metrics_state,0,latency_us);
                        log4c_warn("Exit目标建连注册失败 id=%u connect_ms=%.1f result=epoll-fail error=%s route=%s",
                            p->id,latency_us/1000.0,strerror(epoll_error),p->route);
                        exit_recovery_fail_stream(p,"target-connect-epoll-fail");continue;
                    }
                    if(ps_is_rtc_webcast(p)){ char target[96]; sockaddr_to_text((struct sockaddr*)&r.addr,r.addrlen,target,sizeof(target));
                      log4c_info("exit id=%u target connect result=pending target=%s route=%s",p->id,target,p->route); }
                }
                continue;
            }
            if(tag==NB_EPOLL_TAG_LISTEN){entry_accept();continue;}
            if(tag==NB_EPOLL_TAG_CONTROL){if(nb_control_serve(G.control_fd,control_render,NULL)<0)log4c_warn("control serve failed");continue;}
            if((tag&NB_EPOLL_TAG_UDP_SESSION)!=0){
                size_t index=(size_t)(tag&0xffffu);uint32_t id=(uint32_t)((tag>>16)&0xffffffffu);
                if(index<MAX_CONN){proxy_stream_t* p=&G.streams[index];
                    if(p->in_use&&p->id==id&&p->udp_fd>=0)udp_local_drain(p);}
                continue;
            }
            if((tag&NB_EPOLL_TAG_STREAM)==0)continue;
            size_t index=(size_t)(tag&0xffffu);uint32_t id=(uint32_t)((tag>>16)&0xffffffffu);
            if(index>=MAX_CONN)continue;
            proxy_stream_t* p=&G.streams[index];
            if(!p->in_use||p->id!=id||p->tcp_fd<0)continue;
            if(p->tcp_connecting&&(ee&(EPOLLOUT|EPOLLERR|EPOLLHUP))){
                int se=0;socklen_t sl=sizeof(se);
                if(getsockopt(p->tcp_fd,SOL_SOCKET,SO_ERROR,&se,&sl)!=0)se=errno;
                nb_exit_recovery_action_t connect_action=ps_exit_connect_complete(p,se==0,picoquic_current_time());
                uint64_t latency_us=ps_exit_connect_latency_us(p);
                nb_metrics_note_target_connect(&g_metrics_state,0,latency_us);
                if(connect_action==NB_EXIT_RECOVERY_CONNECT_FAIL){
                    log4c_warn("Exit目标建连失败 id=%u connect_ms=%.1f result=fail error=%s route=%s",
                        p->id,latency_us/1000.0,strerror(se),p->route);
                    exit_recovery_fail_stream(p,"target-connect-fail");continue;
                }
                if(connect_action==NB_EXIT_RECOVERY_CONNECT_OK&&ps_is_rtc_webcast(p))
                    log4c_info("Exit目标建连完成 id=%u connect_ms=%.1f result=ok route=%s",
                        p->id,latency_us/1000.0,p->route);
            }
            if(p->in_use&&!p->tcp_connecting&&!p->tcp_eof&&(ee&(EPOLLIN|EPOLLRDHUP|EPOLLHUP|EPOLLERR))){
                if(p->udp_association){
                    uint8_t control[64];ssize_t n=recv(p->tcp_fd,control,sizeof(control),0);
                    if(n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)){
                        uint64_t now=picoquic_current_time();
                        if(G.epoll_fd>=0)epoll_ctl(G.epoll_fd,EPOLL_CTL_DEL,p->tcp_fd,NULL);
                        close(p->tcp_fd);p->tcp_fd=-1;p->udp_control_closed_at=now;
                        log4c_info("entry UDP ASSOCIATE control closed id=%u grace=%.1fs raw_rx=%llu peer=%s",
                            p->id,g_udp_control_grace_us/1000000.0,
                            (unsigned long long)p->udp_assoc_raw_rx,p->peer_addr);
                    }
                }else if(G.socks_enabled&&p->socks_stage<3){
                    if(p->socks_auth_pending){
                        if(ee&(EPOLLRDHUP|EPOLLHUP|EPOLLERR))ps_teardown_reason(p,"socks-auth-peer-close");
                    }else socks_handshake(p);
                }else pump_tcp(p);
            }
            if(p->in_use&&p->tcp_fd>=0&&(ee&EPOLLOUT))flush_q2t(p);
            if(p->in_use)maybe_free(p);
        }
        exit_connect_expire_due(picoquic_current_time());
        /* middle: 无 TCP 的流也要判释放 */
        for(i=0;i<MAX_CONN;i++){
            proxy_stream_t*p=&G.streams[i];
            if(p->in_use&&p->probe_mode==4&&probe_source_pump(p)!=0)p->need_teardown=1;
            if(p->in_use&&p->tcp_fd<0&&(G.role==ROLE_MIDDLE||p->probe_mode))maybe_free(p);
        }
        /* V1.5 FEC sidecar: 部分 block 超时封口，避免小流量永远攒不满 K 个 symbol 而不发。 */
        { uint64_t fnow=picoquic_current_time();
          for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
            if(!p->in_use || !p->fec_sidecar_mode) continue;
            if(p->fec_engine==NULL){p->need_teardown=1;continue;}
            int rc=nb_fec_tick(p->fec_engine,fnow);
            if(rc!=NB_FEC_OK){ log4c_warn("id=%u fec engine tick fail rc=%d",p->id,rc); p->need_teardown=1; }
            else (void)fec_pump_staged(p);
          } }
        { uint64_t fnow=picoquic_current_time();
          for(i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
            if(p->in_use&&p->udp_fec_tx!=NULL&&udp_fec_flush_due(p,fnow)!=0)
                p->need_teardown=1;
          } }
        /* 过载保护: q2t 超限标记的流, 主循环兜底 teardown(不在回调栈内拆, 避免重入/use-after-free) */
        for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i]; if(p->in_use && p->need_teardown) ps_teardown_reason(p,"internal-error"); }
        /* 空闲超时兜底: 无数据活动超 IDLE_TIMEOUT 的流强制拆除(防僵尸流累积拖死复用连接; 半开/对端不关 FIN 亦兜住) */
        { uint64_t tnow=picoquic_current_time();
          for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i];
            if(p->in_use&&p->udp_association&&p->udp_control_closed_at&&
                nb_udp_control_grace_expired(p->udp_control_closed_at,p->last_active,tnow,g_udp_control_grace_us)){
                log4c_info("entry UDP ASSOCIATE grace expired id=%u idle=%.1fms raw_rx=%llu peer=%s",
                    p->id,(tnow-p->last_active)/1000.0,(unsigned long long)p->udp_assoc_raw_rx,p->peer_addr);
                ps_teardown_reason(p,"udp-control-grace-expired");
            }else if(p->in_use && tnow > p->last_active && tnow - p->last_active > IDLE_TIMEOUT_US){
                log4c_warn("id=%u idle timeout(%llus), teardown route=%s",p->id,
                    (unsigned long long)((tnow-p->last_active)/1000000),p->route);
                ps_teardown_reason(p,"idle-timeout"); } } }
        /* 空队列只保留正常突发所需容量；高水位容量由后续 append 按需重建。 */
        {
          uint64_t memory_now=picoquic_current_time();
          if(memory_now-nb_current_instance->memory_scan_at>=NB_MEMORY_SCAN_INTERVAL_US){
            nb_current_instance->memory_scan_at=memory_now;uint64_t released=0;
            for(i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];if(!p->in_use)continue;
                released+=nb_ring_release_empty(&p->down_tx,NB_EMPTY_QUEUE_RETAIN_MAX);
                released+=nb_ring_release_empty(&p->up_tx,NB_EMPTY_QUEUE_RETAIN_MAX);
                released+=nb_ring_release_empty(&p->q2t,NB_EMPTY_QUEUE_RETAIN_MAX);
            }
            g_memory_released_pending+=released;
#if defined(__GLIBC__)
            if(g_memory_released_pending>=NB_MALLOC_TRIM_MIN_RELEASED&&
                memory_now-nb_current_instance->malloc_trim_at>=NB_MALLOC_TRIM_INTERVAL_US){
                int trimmed=malloc_trim(0);nb_current_instance->malloc_trim_at=memory_now;
                log4c_info("memory reclaim released=%llu malloc_trim=%d",
                    (unsigned long long)g_memory_released_pending,trimmed);g_memory_released_pending=0;
            }
#else
            if(g_memory_released_pending>0){log4c_info("memory reclaim released=%llu",
                (unsigned long long)g_memory_released_pending);g_memory_released_pending=0;}
#endif
          } }
        /* 过载保护: 流表用量观测(每10s), 峰值接近 MAX_CONN 告警, 供扩容/限流决策 */
        { uint64_t tn=picoquic_current_time();
            if(tn - nb_current_instance->stream_usage_last_at > 10000000ULL){ nb_current_instance->stream_usage_last_at=tn;
                if(g_ps_peak*10 >= MAX_CONN*9) log4c_warn("stream table HIGH: inuse=%d peak=%d / max=%d",g_ps_inuse,g_ps_peak,MAX_CONN);
                else log4c_info("stream table: inuse=%d peak=%d / max=%d",g_ps_inuse,g_ps_peak,MAX_CONN); } }
        { uint64_t metrics_now=picoquic_current_time();
          for(i=0;i<MAX_CONN;i++)if(G.streams[i].in_use){
              ps_update_runtime_policy(&G.streams[i],metrics_now);
              sched_refresh_stream(&G.streams[i],metrics_now);
              ps_log_media_metrics(&G.streams[i],metrics_now);
              if(G.streams[i].logical_endpoint&&G.streams[i].logical_rx_delivered>
                  G.streams[i].logical_last_ack_offset&&logical_send_ack(&G.streams[i],0)!=0)
                  ps_teardown_reason(&G.streams[i],"logical-ack-queue");
          }
          if(G.role==ROLE_ENTRY||G.role==ROLE_MIDDLE)pool_health_tick(metrics_now);
          if(G.role==ROLE_ENTRY)logical_reconnect_tick(metrics_now);
        }
        /* 白名单热重载(限流 ~5s 检查一次文件 mtime) */
        { uint64_t tn=picoquic_current_time();
          if(tn-nb_current_instance->whitelist_last_at>5000000ULL){char error[256];nb_current_instance->whitelist_last_at=tn;
              if(nb_whitelist_context_configured(instance->whitelist)&&
                  nb_whitelist_context_reload_if_changed(instance->whitelist,error,sizeof(error))<0)
                  log4c_error("whitelist reload rejected; keeping previous rules: %s",error);
              if(nb_policy_reload_if_changed(instance->policy)<0)
                  log4c_error("flow policy reload rejected; keeping previous rules"); } }
        /* 链路质量观测(每10s): 记录到下一跳各连接的 RTT/丢包, 定位抖动/丢包在哪跳(FEC 动态冗余输入)。
         * entry 观测 entry->middle 段, middle 观测 middle->exit 段, 两段全覆盖。 */
        if(G.pool_count>0){ uint64_t tn=picoquic_current_time();
            if(tn - nb_current_instance->link_quality_last_at > 10000000ULL){ nb_current_instance->link_quality_last_at=tn;
                for(int pool_id=0;pool_id<G.pool_count;pool_id++)for(int k=0;k<POOL_SIZE;k++){
                    cnx_pool_t* pool=&G.pools[pool_id];if(!pool->cnx[k])continue;
                    picoquic_path_quality_t q; memset(&q,0,sizeof(q));
                    picoquic_get_default_path_quality(pool->cnx[k], &q);
                    uint64_t delta_sent = (q.sent >= pool->last_sent_total[k]) ? (q.sent - pool->last_sent_total[k]) : q.sent;
                    uint64_t delta_lost = (q.lost >= pool->last_lost_total[k]) ? (q.lost - pool->last_lost_total[k]) : q.lost;
                    uint64_t delta_timer = (q.timer_losses >= pool->last_timer_total[k])
                        ? (q.timer_losses - pool->last_timer_total[k]) : q.timer_losses;
                    uint64_t delta_spurious = (q.spurious_losses >= pool->last_spurious_total[k])
                        ? (q.spurious_losses - pool->last_spurious_total[k]) : q.spurious_losses;
                    uint64_t delta_repeat = (delta_lost > delta_timer) ? (delta_lost - delta_timer) : 0;
                    uint64_t eff_lost = (delta_lost > delta_spurious) ? (delta_lost - delta_spurious) : 0;
                    picoquic_cnx_t* cnx=pool->cnx[k];
                    uint64_t delta_retrans=(cnx->nb_retransmission_total>=pool->last_retrans_total[k])
                        ? cnx->nb_retransmission_total-pool->last_retrans_total[k] : cnx->nb_retransmission_total;
                    uint64_t delta_preempt=(cnx->nb_preemptive_repeat>=pool->last_preempt_total[k])
                        ? cnx->nb_preemptive_repeat-pool->last_preempt_total[k] : cnx->nb_preemptive_repeat;
                    uint64_t cc_state=0,cc_param=0;
                    const char* cc_alg=(cnx->congestion_alg&&cnx->congestion_alg->congestion_algorithm_id)
                        ? cnx->congestion_alg->congestion_algorithm_id : "none";
                    picoquic_path_t* path=(cnx->nb_paths>0)?cnx->path[0]:NULL;
                    transport_refresh_outbound_seed(cnx,(struct sockaddr*)&pool->addr,q.rtt);
                    if(path&&path->congestion_alg_state&&cnx->congestion_alg&&cnx->congestion_alg->alg_observe)
                        cnx->congestion_alg->alg_observe(path,&cc_state,&cc_param);
                    double loss = delta_sent ? (100.0*(double)eff_lost/(double)delta_sent) : 0.0;
                    pool->recent_loss[k] = loss;
                    pool->recent_rtt[k] = q.rtt;
                    pool->recent_rtt_max[k] = q.rtt_max;
                    pool->recent_sent[k] = delta_sent;
                    pool->last_sent_total[k] = q.sent;
                    pool->last_lost_total[k] = q.lost;
                    pool->last_timer_total[k] = q.timer_losses;
                    pool->last_spurious_total[k] = q.spurious_losses;
                    pool->last_retrans_total[k] = cnx->nb_retransmission_total;
                    pool->last_preempt_total[k] = cnx->nb_preemptive_repeat;
                    pool->recent_rtt_var[k] = q.rtt_variant;
                    pool->recent_ts[k] = tn;
                    int pmtu_event=nb_pmtu_observe(&pool->pmtu[k],path?path->send_mtu:0,
                        delta_timer,delta_spurious,tn);
                    if(pmtu_event>0)g_metrics_state.pmtu_promotions++;
                    else if(pmtu_event<0)g_metrics_state.pmtu_fallbacks++;
                    if(pmtu_event)log4c_warn("pmtu pool[%d:%d] event=%s observed=%zu confirmed=%zu cooldown_until=%llu",
                        pool_id,k,pmtu_event>0?"promote":"blackhole-fallback",path?path->send_mtu:0,
                        nb_pmtu_effective(&pool->pmtu[k]),(unsigned long long)pool->pmtu[k].cooldown_until_us);
                    log4c_info("linkq pool[%d:%d] run=%llx worker=%s cid=%llx mtu=%zu pmtu=%zu rtt=%.1fms(min%.1f/max%.1f) jit=%.1fms loss=%.2f%% dsent=%llu loss_raw=%llu eff=%llu spur=%llu timer=%llu repeat=%llu retx=%llu/%llu pre=%llu/%llu reorder=%.1fms/%llu tol=%llu/%.1fms ack=%.1fms(min%.1f/max%.1f) cc=%s:%s(%llu)/%llu cwin=%lluKB bif=%lluKB block=%u/%u/%u bw=%lluKbps sent=%lluKB",
                        pool_id,k,(unsigned long long)g_run_id,nb_instance_env(nb_current_instance,"NB_WORKER_ID")?nb_instance_env(nb_current_instance,"NB_WORKER_ID"):"0",
                        (unsigned long long)picoquic_val64_connection_id(cnx->initial_cnxid),
                        path?path->send_mtu:0,
                        nb_pmtu_effective(&pool->pmtu[k]),
                        q.rtt/1000.0, q.rtt_min/1000.0, q.rtt_max/1000.0,
                        (double)q.rtt_variant/1000.0, loss,
                        (unsigned long long)delta_sent,
                        (unsigned long long)delta_lost,
                        (unsigned long long)eff_lost,
                        (unsigned long long)delta_spurious,
                        (unsigned long long)delta_timer,
                        (unsigned long long)delta_repeat,
                        (unsigned long long)delta_retrans,(unsigned long long)cnx->nb_retransmission_total,
                        (unsigned long long)delta_preempt,(unsigned long long)cnx->nb_preemptive_repeat,
                        q.max_reorder_delay/1000.0,(unsigned long long)q.max_reorder_gap,
                        (unsigned long long)((cnx->loss_reorder_gap>=3)?cnx->loss_reorder_gap:3),cnx->loss_reorder_delay/1000.0,
                        cnx->ack_delay_remote/1000.0,cnx->min_ack_delay_remote/1000.0,cnx->max_ack_delay_remote/1000.0,
                        cc_alg,cc_state_name(cc_alg,cc_state),(unsigned long long)cc_state,(unsigned long long)cc_param,
                        (unsigned long long)(q.cwin/1024),
                        (unsigned long long)(q.bytes_in_transit/1024),
                        cnx->cwin_blocked,cnx->flow_blocked,cnx->stream_blocked,
                        (unsigned long long)(q.pacing_rate*8/1000),
                        (unsigned long long)(q.bytes_sent/1024));
                    if(G.role==ROLE_MIDDLE&&nb_current_instance->transport_egress.active&&
                        nb_current_instance->transport_egress.link.udp_fec_adaptive)
                        (void)pool_udp_fec_refresh(pool,tn);
                }
            }
        }
        /* 4) QUIC -> UDP (GSO 批量发送, 降 syscall; 不支持时逐段回退) */
        if(g_fec_v15_observe || g_fec_v15_active){
            uint64_t ftn=picoquic_current_time();
            if(ftn - nb_current_instance->fec_stats_last_at > 10000000ULL){ nb_current_instance->fec_stats_last_at=ftn;
                uint64_t ctrl_open=0, ctrl_ready=0, dgq=0, dgs=0, dgr=0, dga=0, dgl=0, stageq=0;
                nb_fec_metrics_t fm=g_fec_metrics_done;
                for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
                    if(!p->in_use || p->fec_session_id==0) continue;
                    ctrl_open += p->fec_ctrl_opened != 0;
                    ctrl_ready += p->fec_ctrl_peer_ready != 0;
                    dgq += (uint64_t)p->fec_dg_tx_len;
                    stageq += (uint64_t)p->fec_stage_tx_len;
                    dgs += p->fec_dg_sent;
                    dgr += p->fec_dg_recv;
                    dga += p->fec_dg_acked;
                    dgl += p->fec_dg_lost;
                    if(p->fec_engine!=NULL) fec_metrics_add(&fm,nb_fec_metrics(p->fec_engine));
                }
                if(ctrl_open || ctrl_ready || dgq || stageq || dgs || dgr || dga || dgl || fm.blocks_encoded || fm.blocks_delivered || g_fec_v15_drop_src || g_fec_v15_drop_repair){
                    double overhead=fm.source_bytes?100.0*(double)fm.repair_bytes/(double)fm.source_bytes:0.0;
                    log4c_info("fec v15 stat: ctrl_open=%llu ctrl_ready=%llu stage_queue=%lluB dg_queue=%lluB sent=%llu recv=%llu dg_acked=%llu dg_lost=%llu encoded=%llu delivered=%llu recovered=%llu recovered_bytes=%llu source_bytes=%llu repair_bytes=%llu overhead=%.1f%% nack=%llu nack_retry=%llu retx_sent=%llu retx_recv=%llu retx_acked=%llu block_acked=%llu ooo=%llu dup=%llu proto_err=%llu window_err=%llu retry_exhausted=%llu drop_src=%llu drop_repair=%llu",
                        (unsigned long long)ctrl_open, (unsigned long long)ctrl_ready,
                        (unsigned long long)stageq, (unsigned long long)dgq,
                        (unsigned long long)dgs, (unsigned long long)dgr, (unsigned long long)dga, (unsigned long long)dgl,
                        (unsigned long long)fm.blocks_encoded,(unsigned long long)fm.blocks_delivered,
                        (unsigned long long)fm.blocks_recovered,(unsigned long long)fm.recovered_source_bytes,
                        (unsigned long long)fm.source_bytes,(unsigned long long)fm.repair_bytes,overhead,
                        (unsigned long long)fm.nack_sent,(unsigned long long)fm.nack_retried,
                        (unsigned long long)fm.retx_sent,(unsigned long long)fm.retx_received,(unsigned long long)fm.retx_acked,
                        (unsigned long long)fm.block_acked,(unsigned long long)fm.out_of_order_packets,
                        (unsigned long long)fm.duplicate_packets,(unsigned long long)fm.protocol_errors,
                        (unsigned long long)fm.window_errors,(unsigned long long)fm.retry_exhausted,
                        (unsigned long long)g_fec_v15_drop_src, (unsigned long long)g_fec_v15_drop_repair);
                }
            }
        }
        udp_send_batch();
        /* 发送和 FEC tick 可能改变队列水位，循环末统一刷新 TCP EPOLLIN/EPOLLOUT。 */
        for(i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];if(p->in_use&&p->tcp_fd>=0)(void)epoll_tcp_update(p,0);}
        { uint64_t loop_end=picoquic_current_time();uint64_t busy=loop_end-busy_started;
          nb_metrics_note_loop(&g_metrics_state,busy,iteration_wake_late);
          loop_iterations++;if(busy>loop_busy_max)loop_busy_max=busy;
          if(busy>=5000)loop_over_5ms++;
          if(busy>=20000)loop_over_20ms++;
          if(loop_end-loop_stat_at>=10000000ULL){
              log4c_info("eventloop stat worker=%s iterations=%llu busy_max=%.1fms wake_late_max=%.1fms over5ms=%llu over20ms=%llu",
                  nb_instance_env(nb_current_instance,"NB_WORKER_ID")?nb_instance_env(nb_current_instance,"NB_WORKER_ID"):"0",
                  (unsigned long long)loop_iterations,loop_busy_max/1000.0,loop_wake_late_max/1000.0,
                  (unsigned long long)loop_over_5ms,(unsigned long long)loop_over_20ms);
              loop_stat_at=loop_end;loop_busy_max=0;loop_wake_late_max=0;
              loop_iterations=0;loop_over_5ms=0;loop_over_20ms=0;
          } }
    }
    atomic_store(&instance->running,0);
    return 0;
}

static void nb_node_instance_cleanup(nb_instance_t* instance){
    if(instance==NULL)return;
    nb_current_instance=instance;
    for(int i=0;i<MAX_CONN;i++)if(G.streams[i].in_use)ps_free(&G.streams[i]);
    nb_auth_async_destroy(instance->auth_async);instance->auth_async=NULL;
    nb_dns_destroy(instance->dns);instance->dns=NULL;
    nb_whitelist_destroy(instance->whitelist);instance->whitelist=NULL;
    nb_policy_destroy(instance->policy);instance->policy=NULL;
    if(g_tenants_enabled){nb_tenants_close(&g_tenants);g_tenants_enabled=0;}
    if(G.quic){picoquic_free(G.quic);G.quic=NULL;}
    int descriptors[]={G.control_fd,G.tcp_listen_fd,G.client_udp_fd,G.udp_fd,G.epoll_fd};
    for(size_t i=0;i<sizeof(descriptors)/sizeof(descriptors[0]);i++)if(descriptors[i]>=0){
        int duplicate=0;for(size_t j=0;j<i;j++)if(descriptors[j]==descriptors[i])duplicate=1;
        if(!duplicate)close(descriptors[i]);
    }
    G.control_fd=G.tcp_listen_fd=G.client_udp_fd=G.udp_fd=G.epoll_fd=-1;
    if(instance->control_path[0])unlink(instance->control_path);
    atomic_store(&instance->running,0);
}

static int nb_node_instance_execute(int argc,char**argv,nb_instance_t* instance){
    int status=nb_node_instance_loop(argc,argv,instance);instance->exit_code=status;
    nb_node_instance_cleanup(instance);if(!instance->managed)log4c_shutdown();return status;
}
