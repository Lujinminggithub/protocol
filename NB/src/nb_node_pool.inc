static picoquic_cnx_t* pool_create_cnx(cnx_pool_t* pool, int idx){
    picoquic_cnx_t* c=picoquic_create_cnx(G.quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&pool->addr, picoquic_current_time(), 0, NB_SNI, NB_ALPN, 1);
    if(c==NULL){ log4c_error("pool[%d] create cnx fail",idx); return NULL; }
    if((G.role==ROLE_ENTRY || G.role==ROLE_MIDDLE) &&
        picoquic_set_loss_reorder_tolerance(c,g_reorder_gap,g_reorder_delay_us)!=0){
        log4c_error("pool[%d] invalid reorder tolerance gap=%llu delay=%lluus",idx,
            (unsigned long long)g_reorder_gap,(unsigned long long)g_reorder_delay_us);
        picoquic_delete_cnx(c); return NULL;
    }
    picoquic_set_callback(c, relay_quic_callback, NULL);
    if(picoquic_start_client_cnx(c)!=0){
        log4c_error("pool[%d] start cnx fail",idx);picoquic_delete_cnx(c);return NULL;
    }
    picoquic_enable_keep_alive(c, 15000000); /* 15s 保活 */
    return c;
}

static int pool_ensure(cnx_pool_t* pool, int idx){
    if(pool->cnx[idx]!=NULL)return 0;
    picoquic_cnx_t* c=pool_create_cnx(pool,idx);if(c==NULL)return -1;
    pool->cnx[idx]=c;pool->next_sid[idx]=0;return 0;
}
/* 初始化连接池(配下一跳地址 + 建满 POOL_SIZE 条)。返回 0=至少一条就绪 -1=全失败 */
static int pool_init(cnx_pool_t* pool, struct sockaddr_storage* addr){
    pool->addr=*addr; pool->rr=0; pool->rr_lat=0;
    memset(pool->cnx,0,sizeof(pool->cnx));memset(pool->replacement_cnx,0,sizeof(pool->replacement_cnx));
    memset(pool->draining_cnx,0,sizeof(pool->draining_cnx));
    memset(pool->replacement_closing,0,sizeof(pool->replacement_closing));
    memset(pool->draining_closing,0,sizeof(pool->draining_closing));
    memset(pool->recent_loss, 0, sizeof(pool->recent_loss));
    memset(pool->recent_rtt, 0, sizeof(pool->recent_rtt));
    memset(pool->recent_rtt_max, 0, sizeof(pool->recent_rtt_max));
    memset(pool->recent_sent, 0, sizeof(pool->recent_sent));
    memset(pool->last_sent_total, 0, sizeof(pool->last_sent_total));
    memset(pool->last_lost_total, 0, sizeof(pool->last_lost_total));
    memset(pool->last_timer_total, 0, sizeof(pool->last_timer_total));
    memset(pool->last_spurious_total, 0, sizeof(pool->last_spurious_total));
    memset(pool->last_retrans_total, 0, sizeof(pool->last_retrans_total));
    memset(pool->last_preempt_total, 0, sizeof(pool->last_preempt_total));
    memset(pool->recent_rtt_var, 0, sizeof(pool->recent_rtt_var));
    memset(pool->recent_ts, 0, sizeof(pool->recent_ts));
    memset(pool->health, 0, sizeof(pool->health));
    for(int i=0;i<POOL_SIZE;i++)nb_pmtu_init(&pool->pmtu[i],
        NB_PMTU_DEFAULT_FLOOR,NB_PMTU_DEFAULT_CEILING);
    pool->fec_latched = 0;
    int ok=0; for(int i=0;i<POOL_SIZE;i++) if(pool_ensure(pool,i)==0) ok++;
    pool->configured=1;
    log4c_info("cnx pool ready: %d/%d connections up",ok,POOL_SIZE);
    return ok>0?0:-1;
}

static int pool_endpoint_equal(const struct sockaddr_storage* a,const struct sockaddr_storage* b){
    if(a->ss_family!=b->ss_family)return 0;
    if(a->ss_family==AF_INET){
        const struct sockaddr_in* x=(const struct sockaddr_in*)a;const struct sockaddr_in* y=(const struct sockaddr_in*)b;
        return x->sin_port==y->sin_port&&x->sin_addr.s_addr==y->sin_addr.s_addr;
    }
    if(a->ss_family==AF_INET6){
        const struct sockaddr_in6* x=(const struct sockaddr_in6*)a;const struct sockaddr_in6* y=(const struct sockaddr_in6*)b;
        return x->sin6_port==y->sin6_port&&memcmp(&x->sin6_addr,&y->sin6_addr,sizeof(x->sin6_addr))==0;
    }
    return 0;
}

static int pool_contains_cnx(picoquic_cnx_t* cnx){
    if(cnx==NULL)return 0;
    for(int pool_id=0;pool_id<G.pool_count;pool_id++){
        for(int idx=0;idx<POOL_SIZE;idx++){
            cnx_pool_t* pool=&G.pools[pool_id];
            if(pool->cnx[idx]==cnx||pool->replacement_cnx[idx]==cnx||
                pool->draining_cnx[idx]==cnx)return 1;
        }
    }
    return 0;
}

static int pool_get_or_create(struct sockaddr_storage* addr){
    for(int i=0;i<G.pool_count;i++)if(G.pools[i].configured&&pool_endpoint_equal(&G.pools[i].addr,addr))return i;
    if(G.pool_count>=NB_MAX_POOLS){log4c_error("next-hop pool registry full max=%d",NB_MAX_POOLS);return -1;}
    int id=G.pool_count;
    if(pool_init(&G.pools[id],addr)!=0)return -1;
    G.pool_count++;
    log4c_info("next-hop pool registered id=%d",id);
    return id;
}

static int pool_pick_all_rr(cnx_pool_t* pool, int avoid_idx){
    for(int k=0;k<POOL_SIZE;k++){
        int idx=pool->rr_lat%POOL_SIZE;pool->rr_lat++;
        if(idx==avoid_idx) continue;
        if(pool->health[idx].quarantined)continue;
        if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
        if(pool->cnx[idx]!=NULL) return idx;
    }
    return -1;
}

#define FEC_LOSS_ON_PCT 3.0
#define FEC_LOSS_OFF_PCT 1.0
#define FEC_JITTER_ON_US 40000ULL
#define FEC_JITTER_OFF_US 15000ULL
#define FEC_MIN_SENT_PKTS 20ULL
#define FEC_COLDSTART_US 3000000ULL
#define REALTIME_LOSS_PRIOR_PCT 2.0
#define REALTIME_HEALTHY_LOSS_PCT 3.0
#define REALTIME_HEALTHY_JITTER_US 40000ULL

static int pool_quality_fresh(cnx_pool_t* pool, int idx, uint64_t now){
    nb_path_quality_t sample={pool->recent_loss[idx],pool->recent_rtt[idx],pool->recent_rtt_var[idx],
        pool->recent_sent[idx],pool->recent_ts[idx],idx};
    return nb_path_quality_fresh(&sample,now,30000000ULL); /* 30s 内样本视为新鲜 */
}

static int pool_quality_sampled(cnx_pool_t* pool, int idx, uint64_t now){
    return pool_quality_fresh(pool, idx, now) && pool->recent_sent[idx] >= FEC_MIN_SENT_PKTS;
}

static double pool_quality_loss_score(cnx_pool_t* pool, int idx){
    nb_path_quality_t sample={pool->recent_loss[idx],pool->recent_rtt[idx],pool->recent_rtt_var[idx],
        pool->recent_sent[idx],pool->recent_ts[idx],idx};
    return nb_path_loss_score(&sample,FEC_MIN_SENT_PKTS,REALTIME_LOSS_PRIOR_PCT);
}

static int pool_quality_better(cnx_pool_t* pool, int a, int b, uint64_t now){
    nb_path_quality_t sa={pool->recent_loss[a],pool->recent_rtt[a],pool->recent_rtt_var[a],pool->recent_sent[a],pool->recent_ts[a],a};
    nb_path_quality_t sb={pool->recent_loss[b],pool->recent_rtt[b],pool->recent_rtt_var[b],pool->recent_sent[b],pool->recent_ts[b],b};
    return nb_path_quality_better(&sa,&sb,now,30000000ULL,FEC_MIN_SENT_PKTS,REALTIME_LOSS_PRIOR_PCT);
}

static int pool_quality_best(cnx_pool_t* pool, int avoid_idx, int require_sampled){
    uint64_t now = picoquic_current_time();
    int best = -1;
    for(int idx=0; idx<POOL_SIZE; idx++){
        if(idx==avoid_idx) continue;
        if(pool->health[idx].quarantined)continue;
        if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
        if(pool->cnx[idx]==NULL) continue;
        if(require_sampled){
            if(!pool_quality_sampled(pool, idx, now)) continue;
        } else {
            if(!pool_quality_fresh(pool, idx, now)) continue;
        }
        if(best<0 || pool_quality_better(pool, idx, best, now)) best = idx;
    }
    return best;
}

/* Q2: 要不要双发？看这条 hop 最近一个采样窗的线质是否变差。
 * 使用"增量 loss + 回滞"版:
 * - 只在样本足够(delta sent >= FEC_MIN_SENT_PKTS)时才更新判断
 * - 打开阈值高于关闭阈值, 避免在边缘抖动时来回开关
 * - 先以全池最优连接的增量 loss/jitter 作为该 hop 当前代表值 */
static int pool_line_bad(cnx_pool_t* pool, int prio, double* best_loss, double* best_jitter_ms, int* best_idx, int* coldstart_on){
    int idx = pool_quality_best(pool, -1, 1);
    int idx2 = (idx >= 0) ? pool_quality_best(pool, idx, 1) : -1;
    if(coldstart_on) *coldstart_on = 0;
    if(best_idx) *best_idx = (idx2 >= 0) ? idx2 : idx;
    if(idx < 0){
        if(best_loss) *best_loss = -1.0;
        if(best_jitter_ms) *best_jitter_ms = -1.0;
        if(prio_is_fec_candidate(prio)){
            int fresh_idx = pool_quality_best(pool, -1, 0);
            if(fresh_idx >= 0 && pool->recent_ts[fresh_idx] != 0 &&
                picoquic_current_time() - pool->recent_ts[fresh_idx] <= FEC_COLDSTART_US){
                if(coldstart_on) *coldstart_on = 1;
                pool->fec_latched = 1;
                if(best_idx) *best_idx = fresh_idx;
                if(best_loss) *best_loss = pool->recent_loss[fresh_idx];
                if(best_jitter_ms) *best_jitter_ms = (double)pool->recent_rtt_var[fresh_idx] / 1000.0;
                return 1;
            }
        }
        return pool->fec_latched;
    }
    int eval_idx = (idx2 >= 0) ? idx2 : idx; /* 用前两条最优里的第二优值, 比单看最优更保守 */
    double loss = pool->recent_loss[eval_idx];
    double jitter_ms = (double)pool->recent_rtt_var[eval_idx] / 1000.0;
    if(best_loss) *best_loss = loss;
    if(best_jitter_ms) *best_jitter_ms = jitter_ms;
    if(idx2 < 0){
        /* 只有一条样本充足的连接时, 允许它触发打开, 但不允许它单独触发关闭。 */
        if(loss >= FEC_LOSS_ON_PCT || pool->recent_rtt_var[eval_idx] >= FEC_JITTER_ON_US){
            pool->fec_latched = 1;
            return 1;
        }
        if(prio_is_fec_candidate(prio) && pool->recent_ts[eval_idx] != 0 &&
            picoquic_current_time() - pool->recent_ts[eval_idx] <= FEC_COLDSTART_US){
            if(coldstart_on) *coldstart_on = 1;
            pool->fec_latched = 1;
            return 1;
        }
        return pool->fec_latched;
    }
    if(!pool->fec_latched){
        if(loss >= FEC_LOSS_ON_PCT || pool->recent_rtt_var[eval_idx] >= FEC_JITTER_ON_US) pool->fec_latched = 1;
    } else {
        if(loss <= FEC_LOSS_OFF_PCT && pool->recent_rtt_var[eval_idx] <= FEC_JITTER_OFF_US) pool->fec_latched = 0;
    }
    return pool->fec_latched;
}

/* FEC 从全池挑当前最稳连接；优先使用 30s 内有效样本，无样本时回退全池轮询。 */
static int pool_pick_fec_best(cnx_pool_t* pool, int avoid_idx){
    uint64_t now = picoquic_current_time();
    int best = -1;
    for(int idx=0; idx<POOL_SIZE; idx++){
        if(idx==avoid_idx) continue;
        if(pool->health[idx].quarantined)continue;
        if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
        if(pool->cnx[idx]==NULL) continue;
        if(!pool_quality_sampled(pool, idx, now)) continue;
        if(best<0 || pool_quality_better(pool, idx, best, now)) best = idx;
    }
    if(best>=0) return best;
    best = pool_quality_best(pool, avoid_idx, 0);
    return best>=0 ? best : pool_pick_all_rr(pool, avoid_idx);
}

static int pool_pick_latency_quality(cnx_pool_t* pool){
    int trusted=pool_quality_best(pool,-1,1);
    if(trusted>=0&&pool->recent_loss[trusted]<=REALTIME_HEALTHY_LOSS_PCT&&
        pool->recent_rtt_var[trusted]<=REALTIME_HEALTHY_JITTER_US){
        int second=pool_quality_best(pool,trusted,1);
        if(second>=0&&pool->recent_loss[second]<=REALTIME_HEALTHY_LOSS_PCT&&
            pool_quality_loss_score(pool,second)<=pool_quality_loss_score(pool,trusted)+1.0&&
            pool->recent_rtt_var[second]<=pool->recent_rtt_var[trusted]+10000ULL){
            return (pool->rr_lat++&1)?second:trusted;
        }
        return trusted;
    }
    int best=pool_quality_best(pool,-1,0);
    if(best<0)return pool_pick_all_rr(pool,-1);
    int second=pool_quality_best(pool,best,0);
    if(second>=0&&pool_quality_loss_score(pool,second)<=pool_quality_loss_score(pool,best)+1.0&&
        pool->recent_rtt_var[second]<=pool->recent_rtt_var[best]+10000ULL){
        return (pool->rr_lat++&1)?second:best;
    }
    return best;
}

/* 实时流从全池按 loss -> jitter -> RTT 选路；批量流仍优先后半池。 */
static int pool_pick(cnx_pool_t* pool, int is_latency){
    if(is_latency){
        return pool_pick_latency_quality(pool);
    } else if(POOL_SIZE>BULK_POOL_START){
        for(int k=BULK_POOL_START;k<POOL_SIZE;k++){
            int idx=BULK_POOL_START+(pool->rr % (POOL_SIZE-BULK_POOL_START)); pool->rr++;
            if(pool->health[idx].quarantined)continue;
            if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
            if(pool->cnx[idx]!=NULL) return idx;
        }
    }
    for(int k=0;k<POOL_SIZE;k++){
        if(pool->health[k].quarantined)continue;
        if(pool->cnx[k]==NULL)pool_ensure(pool,k);
        if(pool->cnx[k]!=NULL)return k;
    }
    return -1;
}

/* 分配下游 stream 并发首部(带 <prio>; 标记端到端传递)+ 设该 stream 优先级。 */
static uint64_t pool_slot_queue_age(picoquic_cnx_t* cnx,uint64_t now,int* eligible){
    uint64_t oldest=0;*eligible=0;
    for(int i=0;i<MAX_CONN;i++){
        proxy_stream_t* p=&G.streams[i];if(!p->in_use||p->down_cnx!=cnx)continue;
        if(p->flow_class!=NB_FLOW_CLASS_MEDIA&&p->lane_hint!=NB_FLOW_LANE_LATENCY&&
            p->probe_mode==0&&strstr(p->route,"nb-probe-")==NULL)continue;
        uint64_t age=0;
        if(p->udp_mode){
            const uint8_t* data=NULL;size_t length=0;uint64_t queued_at=0;
            if(dgramq_peek(p->udp_down_tx,p->udp_down_tx_len,&data,&length,&queued_at)&&
                queued_at&&now>=queued_at)age=now-queued_at;
        }else age=nb_live_queue_age_us(&p->down_tx_clock,p->down_tx.len,now);
        if(age>oldest)oldest=age;
        (*eligible)++;
    }
    return oldest;
}
static int pool_slot_streams(picoquic_cnx_t* cnx){int n=0;for(int i=0;i<MAX_CONN;i++)if(G.streams[i].in_use&&G.streams[i].down_cnx==cnx)n++;return n;}

static int pool_cnx_ready(picoquic_cnx_t* cnx){
    if(cnx==NULL)return 0;
    picoquic_state_enum state=picoquic_get_cnx_state(cnx);
    return state==picoquic_state_client_ready_start||state==picoquic_state_ready;
}

static int pool_start_replacement(cnx_pool_t* pool,int pool_id,int idx){
    if(pool->replacement_cnx[idx]!=NULL)return 0;
    picoquic_cnx_t* replacement=pool_create_cnx(pool,idx);if(replacement==NULL)return -1;
    pool->replacement_cnx[idx]=replacement;pool->replacement_closing[idx]=0;
    log4c_warn("pool[%d:%d] quarantined replacement=started",pool_id,idx);return 0;
}

static void pool_close_candidate(cnx_pool_t* pool,int idx){
    if(pool->replacement_cnx[idx]&&!pool->replacement_closing[idx]){
        pool->replacement_closing[idx]=1;picoquic_close(pool->replacement_cnx[idx],0x4e4243ULL);
    }
}

static int pool_migrate_logical(picoquic_cnx_t* old){
    if(G.role!=ROLE_ENTRY||old==NULL)return 0;
    int migrated=0;
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p->in_use&&p->logical_endpoint&&p->down_cnx==old){logical_detach(p,old);migrated++;}}
    return migrated;
}

static int pool_promote_replacement(cnx_pool_t* pool,int pool_id,int idx){
    picoquic_cnx_t* replacement=pool->replacement_cnx[idx];
    if(!pool_cnx_ready(replacement)||pool->draining_cnx[idx]!=NULL)return 0;
    picoquic_cnx_t* old=pool->cnx[idx];pool->draining_cnx[idx]=old;pool->draining_closing[idx]=0;
    pool->cnx[idx]=replacement;pool->replacement_cnx[idx]=NULL;pool->replacement_closing[idx]=0;
    pool->next_sid[idx]=0;nb_pool_slot_reset_quality(pool,idx);memset(&pool->health[idx],0,sizeof(pool->health[idx]));
    g_metrics_state.pool_mbb_promotions++;
    int old_streams=pool_slot_streams(old);int migrated=pool_migrate_logical(old);
    log4c_warn("pool[%d:%d] make-before-break promoted old_streams=%d logical_migrated=%d",pool_id,idx,old_streams,migrated);
    return 1;
}

static void pool_drain_tick(cnx_pool_t* pool,int pool_id,int idx){
    picoquic_cnx_t* draining=pool->draining_cnx[idx];if(draining==NULL)return;
    int streams=pool_slot_streams(draining);
    if(streams==0&&!pool->draining_closing[idx]){
        pool->draining_closing[idx]=1;g_metrics_state.pool_retire_total++;
        log4c_info("pool[%d:%d] draining connection complete",pool_id,idx);
        picoquic_close(draining,0x4e4244ULL);
    }
}

static void pool_health_tick(uint64_t now){
    static uint64_t last=0;if(last&&now-last<250000ULL)return;last=now;
    for(int pool_id=0;pool_id<G.pool_count;pool_id++)for(int idx=0;idx<POOL_SIZE;idx++){
        cnx_pool_t* pool=&G.pools[pool_id];pool_drain_tick(pool,pool_id,idx);
        picoquic_cnx_t* cnx=pool->cnx[idx];if(cnx==NULL)continue;
        int eligible=0;uint64_t age=pool_slot_queue_age(cnx,now,&eligible);
        picoquic_path_t* path=cnx->nb_paths?cnx->path[0]:NULL;uint64_t pto=path?path->nb_retransmit:0,suppressed=0;
        int streams=pool_slot_streams(cnx);
        int connection_candidate=pto>=2;
        int health_eligible=(eligible>0&&streams>0)||connection_candidate;
        uint64_t health_age=connection_candidate&&age<g_pool_health_cfg.queue_age_threshold_us
            ?g_pool_health_cfg.queue_age_threshold_us:age;
        int blocked=cnx->cwin_blocked||cnx->stream_blocked;if(G.role==ROLE_MIDDLE)blocked=blocked||cnx->flow_blocked||age>=g_pool_health_cfg.queue_age_threshold_us*2;
        /* 连续 PTO 可处理无活动 stream 的失效槽，但仍须同时通过 ACK 进展、hold 和冷却门槛。 */
        int was_quarantined=pool->health[idx].quarantined;
        int retire=nb_pool_health_evaluate(pool,idx,&g_pool_health_cfg,now,health_eligible,
            health_age,blocked||connection_candidate,path?path->delivered:0,&suppressed);
        if(!was_quarantined&&pool->health[idx].quarantined)g_metrics_state.pool_quarantine_total++;
        int replacement_ready=pool_cnx_ready(pool->replacement_cnx[idx]);
        nb_pool_recovery_action_t action=nb_pool_health_recovery_action(&pool->health[idx],
            pool->replacement_cnx[idx]!=NULL,replacement_ready,pool->draining_cnx[idx]!=NULL);
        if(action==NB_POOL_RECOVERY_START_REPLACEMENT)(void)pool_start_replacement(pool,pool_id,idx);
        else if(action==NB_POOL_RECOVERY_PROMOTE_REPLACEMENT){if(pool_promote_replacement(pool,pool_id,idx))continue;}
        else if(action==NB_POOL_RECOVERY_CLOSE_REPLACEMENT)pool_close_candidate(pool,idx);
        if(retire)log4c_warn("pool[%d:%d] retire deferred queue_age=%.1fms pto=%llu streams=%d replacement_ready=%d",
            pool_id,idx,age/1000.0,(unsigned long long)pto,streams,replacement_ready);
        g_metrics_state.pool_retire_suppressed+=suppressed;
    }
}

static int downstream_open_stream(proxy_stream_t* p, const char* route, const nb_flow_policy_t* pol){
    if(p->down_pool_id<0||p->down_pool_id>=G.pool_count){log4c_error("id=%u has no downstream pool",p->id);return -1;}
    if(G.role==ROLE_ENTRY&&!p->udp_mode&&p->probe_mode==0&&!p->logical_endpoint&&logical_entry_init(p)!=0){
        log4c_error("id=%u logical flow allocation failed",p->id);return -1;}
    cnx_pool_t* pool=&G.pools[p->down_pool_id];
    int prio = pol ? pol->prio : p->prio;
    double best_loss=-1.0, best_jitter_ms=-1.0;
    int best_idx=-1;
    int coldstart_on=0;
    int line_bad = ((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) &&
        (g_fec_v15_observe || g_fec_v15_active))
        ? pool_line_bad(pool, prio, &best_loss, &best_jitter_ms, &best_idx, &coldstart_on) : 0;
    int latency_hint = pol ? (pol->lane_hint == NB_FLOW_LANE_LATENCY) : prio_is_latency(prio);
    int v15_prio_ok = pol ? (pol->fec_hint == NB_FLOW_FEC_FORCE_ON || pol->fec_hint == NB_FLOW_FEC_AUTO) : prio_is_fec_candidate(prio);
    int fec_v15_would_enable = (G.role==ROLE_MIDDLE && g_fec_v15_observe && v15_prio_ok &&
        (line_bad || g_fec_v15_force));
    int fec_v15 = (g_fec_v15_active && fec_v15_would_enable && !p->logical_mode);
    if(pol) apply_flow_policy(p, pol);
    if((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) && latency_hint){
        log4c_info("%s id=%u fec gate route=%s class=%s lane=%s fec_hint=%s rule=%s prio=%d line_bad=%d coldstart=%d best=%d best_loss=%.2f best_jitter=%.1fms v15_active=%d v15_would=%d",
            role_name(G.role), p->id, route,
            pol ? nb_flow_class_name(pol->flow_class) : nb_flow_class_name(p->flow_class),
            pol ? nb_flow_lane_name(pol->lane_hint) : nb_flow_lane_name(p->lane_hint),
            pol ? nb_flow_fec_name(pol->fec_hint) : nb_flow_fec_name(p->fec_hint),
            pol ? pol->rule_name : p->flow_rule,
            prio, line_bad, coldstart_on, best_idx, best_loss, best_jitter_ms, fec_v15,
            fec_v15_would_enable);
    }

    int idx = fec_v15 ? pool_pick_fec_best(pool, -1) : pool_pick(pool, latency_hint);
    if(idx<0){ log4c_error("id=%u no downstream cnx in pool", p->id); return -1; }
    if(latency_hint){
        log4c_info("%s id=%u realtime pool select=%d sampled=%d dsent=%llu loss=%.2f score=%.2f jitter=%.1fms rtt=%.1fms",
            role_name(G.role),p->id,idx,pool_quality_sampled(pool,idx,picoquic_current_time()),
            (unsigned long long)pool->recent_sent[idx],pool->recent_loss[idx],pool_quality_loss_score(pool,idx),
            pool->recent_rtt_var[idx]/1000.0,pool->recent_rtt[idx]/1000.0);
    }
    picoquic_cnx_t* c=pool->cnx[idx];
    p->down_cnx=c; p->prio=prio; p->down_pool_idx=idx;
    if(pol&&pol->flow_class==NB_FLOW_CLASS_MEDIA)picoquic_set_media_mode(c,1);
    p->down_stream_id=pool->next_sid[idx]; pool->next_sid[idx]+=4; /* 该连接独立的 client bidi */
    ps_index_refresh(p);
    p->downstream_open_at=picoquic_current_time();
    picoquic_set_app_stream_ctx(c,p->down_stream_id,p);
    picoquic_set_stream_priority(c,p->down_stream_id,
        (uint8_t)(latency_hint ? NB_ROUTE_BOOTSTRAP_PRIO : prio));
    if(fec_v15){
        nb_fec_profile_t profile=nb_fec_profile_select(best_loss,best_jitter_ms,coldstart_on,g_fec_v15_force_robust);
        p->fec_sidecar_mode = 1;
        p->fec_session_id=fec_session_id_new();
        if(p->fec_session_id==0){log4c_error("id=%u unable to allocate fec session id",p->id);return -1;}
        p->fec_ctrl_cnx = c;
        p->fec_ctrl_stream_id = p->down_stream_id;
        p->fec_ctrl_opened = 1;
        p->fec_dg_cnx = c;
        snprintf(p->fec_route, sizeof(p->fec_route), "%s", route);
        p->down_opened=1;
        if(nb_fec_config_for_bdp(&p->fec_config,profile.k,profile.r,g_fec_bdp_bitrate_bps,
            g_fec_bdp_rtt_us,g_fec_bdp_multiplier)!=NB_FEC_OK)return -1;
        if(fec_engine_ensure(p)!=0) return -1;
        {
            char ctrl[420];
            int cl=nb_fec_start_render(ctrl,sizeof(ctrl),p->fec_session_id,prio,profile.k,profile.r,route);
            if(cl<=0||(size_t)cl>=sizeof(ctrl)||queue_down_prefix(p,(const uint8_t*)ctrl,(size_t)cl)!=0){
                log4c_error("id=%u queue fec ctrl start fail", p->id); return -1; }
        }
        log4c_info("middle id=%u fec v1.5 start sid=%u ctrl_sid=%llu profile=%s k=%u r=%u route=%s loss=%.2f jitter=%.1fms",
            p->id, p->fec_session_id, (unsigned long long)p->down_stream_id,
            nb_fec_profile_name(profile.level),profile.k,profile.r,route,best_loss,best_jitter_ms);
        return 0;
    }
    p->down_opened=1;
    if(G.role==ROLE_ENTRY&&p->logical_endpoint){
        int resume=p->logical_reconnect_pending;
        uint8_t bootstrap[768];
        int length=nb_lstream_bootstrap_encode(bootstrap,sizeof(bootstrap),prio,route,
            resume?NB_LSTREAM_RESUME:NB_LSTREAM_OPEN,p->logical_flow_hi,p->logical_flow_lo,
            resume?p->logical_rx_delivered:0);
        if(length<0||queue_down_prefix(p,bootstrap,(size_t)length)!=0){
            log4c_error("id=%u queue logical route bootstrap fail",p->id);return -1;}
        p->logical_reconnect_pending=0;p->logical_disconnected_at=0;p->logical_reconnect_last_attempt=0;
    }else{
        char hdr[390],flow[33];int hl=0;
        if(p->logical_mode&&nb_lstream_flow_render(flow,p->logical_flow_hi,p->logical_flow_lo)==0)
            hl=snprintf(hdr,sizeof(hdr),"%d;L=%s;%s\n",prio,flow,route);
        else hl=snprintf(hdr,sizeof(hdr),"%d;%s\n",prio,route);
        if(hl<=0||(size_t)hl>=sizeof(hdr)||queue_down_prefix(p,(const uint8_t*)hdr,(size_t)hl)!=0){
            log4c_error("id=%u queue route to downstream fail", p->id); return -1; }
    }
    if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=stream-open age=%.1fms request_to_open=%.1fms sid=%llu route=%s",
        role_name(G.role),p->id,trace_delta_ms(p->downstream_open_at,p->created_at),
        trace_delta_ms(p->downstream_open_at,p->socks_request_at),(unsigned long long)p->down_stream_id,p->route);
    return 0;
}

static void logical_reconnect_tick(uint64_t now){
    if(G.role!=ROLE_ENTRY)return;
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(!p->in_use||!p->logical_endpoint||!p->logical_reconnect_pending||p->down_cnx!=NULL)continue;
        if(p->logical_reconnect_last_attempt&&now-p->logical_reconnect_last_attempt<250000ULL)continue;
        p->logical_reconnect_last_attempt=now;
        if(p->logical_disconnected_at&&now>p->logical_disconnected_at&&
            now-p->logical_disconnected_at>60000000ULL){
            ps_teardown_reason(p,"logical-resume-timeout");continue;}
        nb_flow_policy_t pol;nb_flow_policy_default(&pol);pol.prio=p->prio;pol.flow_class=p->flow_class;
        pol.lane_hint=p->lane_hint;pol.fec_hint=p->fec_hint;snprintf(pol.rule_name,sizeof(pol.rule_name),"%s",p->flow_rule);
        if(downstream_open_stream(p,p->route,&pol)==0)
            log4c_warn("entry logical flow resumed id=%u flow=%016llx%016llx sid=%llu",
                p->id,(unsigned long long)p->logical_flow_hi,(unsigned long long)p->logical_flow_lo,
                (unsigned long long)p->down_stream_id);
        else log4c_warn("entry logical flow resume deferred id=%u flow=%016llx%016llx",
            p->id,(unsigned long long)p->logical_flow_hi,(unsigned long long)p->logical_flow_lo);
    }
}

static void fwd_down(proxy_stream_t* p, uint8_t* d, size_t n, int fin){
    ps_touch(p);
    ps_note_payload(p, 1, n);
    if(G.role==ROLE_ENTRY&&p->logical_endpoint){
        if(logical_send_data(p,1,d,n,fin)!=0)p->need_teardown=1;
    } else if(G.role==ROLE_EXIT){
        if(q2t_append(p,d,n)!=0){ p->need_teardown=1; return; }
        if(fin) p->q2t_fin=1;
    } else {
        if(G.role==ROLE_MIDDLE && p->fec_sidecar_mode){
            if(fec_stage_bytes(p, d, n, fin) != 0) return;
            (void)fec_pump_staged(p);
            return;
        }
        if(n||fin){
            if(queue_down(p,d,n,fin)!=0) return;
        }
    }
}
static void fwd_up(proxy_stream_t* p, uint8_t* d, size_t n, int fin){
    ps_touch(p);
    ps_note_payload(p, 0, n);
    if(G.role==ROLE_EXIT&&p->logical_endpoint){
        if(logical_send_data(p,0,d,n,fin)!=0)p->need_teardown=1;
    } else if(G.role==ROLE_ENTRY){
        if(q2t_append(p,d,n)!=0){ p->need_teardown=1; return; }
        if(fin) p->q2t_fin=1;
    } else { /* middle / exit: 写回上游 QUIC stream */
        if(G.role==ROLE_EXIT && p->fec_sidecar_mode){
            if(fec_stage_bytes(p, d, n, fin) != 0) return;
            (void)fec_pump_staged(p);
            return;
        }
        if(n||fin){
            if(queue_up(p,d,n,fin)!=0) return;
        }
    }
}

static proxy_stream_t* udp_middle_open(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view,
    const char* route,char* rest,size_t rest_cap){
    char next_host[256];int next_port=0;
    if(udp_route_consume_h(route,next_host,sizeof(next_host),&next_port,rest,rest_cap)!=0)return NULL;
    next_port=worker_lane_port(next_port);if(next_port<1)return NULL;
    struct sockaddr_storage addr;int is_name=0;
    if(picoquic_get_server_address(next_host,next_port,&addr,&is_name)!=0)return NULL;
    proxy_stream_t* p=ps_alloc();if(p==NULL)return NULL;p->udp_mode=1;p->udp_session_id=view->session_id;
    p->up_cnx=cnx;p->down_pool_id=pool_get_or_create(&addr);
    char target[256];int target_port=0;nb_flow_policy_t pol;
    if(parse_target_from_route(rest,target,sizeof(target),&target_port)==0){
        flow_policy_for_host(target,target_port,&pol);apply_flow_policy(p,&pol);
        snprintf(p->udp_target_host,sizeof(p->udp_target_host),"%s",target);p->udp_target_port=target_port;
    }else nb_flow_policy_default(&pol);
    if(p->down_pool_id<0){ps_set_close_reason(p,"udp-middle-pool-fail");ps_free(p);return NULL;}
    cnx_pool_t* pool=&G.pools[p->down_pool_id];int idx=pool_pick(pool,pol.lane_hint==NB_FLOW_LANE_LATENCY);
    if(idx<0){ps_set_close_reason(p,"udp-middle-no-path");ps_free(p);return NULL;}
    p->down_cnx=pool->cnx[idx];p->down_pool_idx=idx;snprintf(p->route,sizeof(p->route),"%s",rest);
    ps_index_refresh(p);
    if(pol.flow_class==NB_FLOW_CLASS_MEDIA)picoquic_set_media_mode(p->down_cnx,1);
    log4c_info("middle udp flow open id=%u usid=%u target=%s:%d route=%s",
        p->id,p->udp_session_id,p->udp_target_host,p->udp_target_port,p->route);return p;
}
