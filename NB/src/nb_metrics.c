#include "nb_metrics.h"

#include <stdio.h>
#include <string.h>

#include "nb_policy.h"

static void add_saturated(uint64_t* total,uint64_t value){
    *total=UINT64_MAX-*total<value?UINT64_MAX:*total+value;
}

void nb_metrics_note_traffic(nb_metrics_state_t* state,uint64_t bytes_c2s,uint64_t bytes_s2c){
    if(state==NULL)return;
    add_saturated(&state->bytes_c2s,bytes_c2s);
    add_saturated(&state->bytes_s2c,bytes_s2c);
}

void nb_metrics_note_close(nb_metrics_state_t* state,const char* reason){
    if(state==NULL)return;
    state->close_total++;
    if(reason&&strstr(reason,"timeout"))state->close_timeout++;
    else if(reason&&(strstr(reason,"reset")||strstr(reason,"stop-sending")||strstr(reason,"connection-close")))state->close_reset++;
    else if(reason&&(strstr(reason,"fail")||strstr(reason,"error")||strstr(reason,"overflow")||strstr(reason,"malformed")||strstr(reason,"reject")))state->close_error++;
    else state->close_normal++;
}

void nb_metrics_note_loop(nb_metrics_state_t* state,uint64_t busy_us,uint64_t wake_late_us){
    if(state==NULL)return;
    state->loop_iterations++;
    if(busy_us>5000)state->loop_over_5ms++;
    if(busy_us>20000)state->loop_over_20ms++;
    if(busy_us>state->loop_busy_max_us)state->loop_busy_max_us=busy_us;
    if(wake_late_us>state->loop_wake_late_max_us)state->loop_wake_late_max_us=wake_late_us;
}

void nb_metrics_note_udp_error(nb_metrics_state_t* state,int transmit){
    if(state==NULL)return;
    if(transmit)state->udp_tx_errors++;else state->udp_rx_errors++;
}

void nb_metrics_note_udp_rxq_overflow(nb_metrics_state_t* state,uint64_t dropped){
    if(state==NULL)return;
    if(UINT64_MAX-state->udp_rxq_overflow<dropped)state->udp_rxq_overflow=UINT64_MAX;
    else state->udp_rxq_overflow+=dropped;
}

void nb_metrics_note_udp_queue_pressure_drop(nb_metrics_state_t* state,uint64_t dropped){
    if(state==NULL)return;
    if(UINT64_MAX-state->udp_queue_pressure_dropped<dropped)state->udp_queue_pressure_dropped=UINT64_MAX;
    else state->udp_queue_pressure_dropped+=dropped;
}

void nb_metrics_note_dns(nb_metrics_state_t* state,int failed,int private_rejected,uint64_t latency_us){
    if(state==NULL)return;
    add_saturated(&state->dns_requests,1);
    if(failed)add_saturated(&state->dns_failures,1);
    if(private_rejected)add_saturated(&state->dns_private_rejected,1);
    if(latency_us>state->dns_latency_max_us)state->dns_latency_max_us=latency_us;
}

void nb_metrics_note_target_connect(nb_metrics_state_t* state,int timeout,uint64_t latency_us){
    if(state==NULL)return;
    if(timeout)add_saturated(&state->target_connect_timeouts,1);
    if(latency_us>state->target_connect_latency_max_us)state->target_connect_latency_max_us=latency_us;
}

void nb_metrics_snapshot_init(nb_metrics_snapshot_t* snapshot,const nb_metrics_state_t* state,
    uint64_t sessions,uint64_t sessions_peak,uint64_t pools,uint64_t exit_routes){
    if(snapshot==NULL)return;
    memset(snapshot,0,sizeof(*snapshot));
    snapshot->sessions=sessions;snapshot->sessions_peak=sessions_peak;
    snapshot->pools=pools;snapshot->exit_routes=exit_routes;
    if(state){snapshot->lifetime=*state;snapshot->bytes_c2s=state->bytes_c2s;
        snapshot->bytes_s2c=state->bytes_s2c;}
}

void nb_metrics_snapshot_add_stream(nb_metrics_snapshot_t* s,int flow_class,int udp_mode,
    int tcp_connecting,int tcp_read_paused,int upstream_fc_blocked,int target_connect_failed,
    uint64_t bytes_c2s,uint64_t bytes_s2c,size_t qd,size_t qu,size_t q2t,
    uint64_t ad,uint64_t au,uint64_t aq,uint64_t first_c2s,uint64_t first_s2c){
    if(s==NULL)return;
    if(flow_class==NB_FLOW_CLASS_CTRL)s->ctrl_sessions++;
    else if(flow_class==NB_FLOW_CLASS_MEDIA)s->media_sessions++;
    else if(flow_class==NB_FLOW_CLASS_BULK)s->bulk_sessions++;
    else s->unknown_sessions++;
    s->udp_sessions+=udp_mode!=0;s->tcp_connecting+=tcp_connecting!=0;
    s->tcp_read_paused+=tcp_read_paused!=0;s->upstream_fc_blocked+=upstream_fc_blocked!=0;
    s->target_connect_failed+=target_connect_failed!=0;
    if(first_c2s>s->first_c2s_wait_max_us)s->first_c2s_wait_max_us=first_c2s;
    if(first_s2c>s->first_s2c_wait_max_us)s->first_s2c_wait_max_us=first_s2c;
    s->bytes_c2s+=bytes_c2s;s->bytes_s2c+=bytes_s2c;
    s->queue_down_bytes+=qd;s->queue_up_bytes+=qu;s->queue_q2t_bytes+=q2t;
    if(ad>s->queue_down_age_max_us)s->queue_down_age_max_us=ad;
    if(au>s->queue_up_age_max_us)s->queue_up_age_max_us=au;
    if(aq>s->queue_q2t_age_max_us)s->queue_q2t_age_max_us=aq;
}

void nb_metrics_snapshot_add_link(nb_metrics_snapshot_t* s,double loss,uint64_t rtt,uint64_t jitter,uint64_t sent){
    if(s==NULL||sent==0)return;
    s->link_samples++;s->link_sent_packets+=sent;
    if(loss>s->link_effective_loss_max_pct)s->link_effective_loss_max_pct=loss;
    if(rtt>s->link_rtt_max_us)s->link_rtt_max_us=rtt;
    if(jitter>s->link_jitter_max_us)s->link_jitter_max_us=jitter;
}

void nb_metrics_snapshot_add_transport(nb_metrics_snapshot_t* s,const char* cc,uint64_t state,
    uint64_t lost,uint64_t spurious,uint64_t timer,uint64_t gap,uint64_t delay,
    uint64_t cwin,uint64_t inflight,uint64_t pacing,int blocked){
    if(s==NULL)return;
    s->link_lost_total+=lost;s->link_spurious_total+=spurious;s->link_timer_loss_total+=timer;
    if(gap>s->link_reorder_gap_max)s->link_reorder_gap_max=gap;
    if(delay>s->link_reorder_delay_max_us)s->link_reorder_delay_max_us=delay;
    if(cwin>s->link_cwin_max_bytes)s->link_cwin_max_bytes=cwin;
    if(inflight>s->link_bytes_in_flight_max)s->link_bytes_in_flight_max=inflight;
    if(pacing>s->link_pacing_rate_max)s->link_pacing_rate_max=pacing;
    s->link_blocked_connections+=blocked!=0;
    if(s->link_cc[0]==0&&cc){snprintf(s->link_cc,sizeof(s->link_cc),"%s",cc);s->link_cc_state=state;}
}

void nb_metrics_snapshot_add_seed(nb_metrics_snapshot_t* s,int applied,uint64_t rtt,uint64_t cwin){
    if(s==NULL)return;
    s->bdp_seed_configured++;
    s->bdp_seed_applied+=applied!=0;
    if(rtt>s->bdp_seed_rtt_max_us)s->bdp_seed_rtt_max_us=rtt;
    if(cwin>s->bdp_seed_cwin_max_bytes)s->bdp_seed_cwin_max_bytes=cwin;
}

int nb_metrics_render_json(char* out,size_t cap,const char* role,const char* worker,
    const char* release,const char* profile,int schema,const nb_metrics_snapshot_t* s,const nb_metrics_fec_t* f){
    if(out==NULL||cap==0||s==NULL||f==NULL)return -1;
    char yfe2[4096];
    if(nb_yfe2_metrics_render_json(yfe2,sizeof(yfe2),&f->nb_yfe2)<0)return -1;
    return snprintf(out,cap,
        "{\"role\":\"%s\",\"worker\":\"%s\",\"release_id\":\"%s\",\"line_profile\":\"%s\",\"line_profile_schema\":%d,"
        "\"sessions\":%llu,\"sessions_peak\":%llu,\"sessions_limit\":%llu,\"pools\":%llu,\"exit_routes\":%llu,"
        "\"instance_resources\":{\"queue_bytes_used\":%llu,\"queue_bytes_limit\":%llu},"
        "\"flow_sessions\":{\"ctrl\":%llu,\"media\":%llu,\"bulk\":%llu,\"unknown\":%llu,\"udp\":%llu},"
        "\"state\":{\"tcp_connecting\":%llu,\"tcp_read_paused\":%llu,\"upstream_fc_blocked\":%llu,\"target_connect_failed\":%llu},"
        "\"exit_connectivity\":{\"dns_requests\":%llu,\"dns_failures\":%llu,\"dns_private_rejected\":%llu,\"dns_latency_max_us\":%llu,\"target_connect_timeouts\":%llu,\"target_connect_latency_max_us\":%llu},"
        "\"first_byte_wait_max_us\":{\"c2s\":%llu,\"s2c\":%llu},"
        "\"bytes\":{\"c2s\":%llu,\"s2c\":%llu},"
        "\"queue_bytes\":{\"down\":%llu,\"up\":%llu,\"q2t\":%llu},"
        "\"queue_age_max_us\":{\"down\":%llu,\"up\":%llu,\"q2t\":%llu},"
        "\"link\":{\"samples\":%llu,\"sent_packets\":%llu,\"effective_loss_max_pct\":%.3f,\"rtt_max_us\":%llu,\"jitter_max_us\":%llu,"
        "\"lost_total\":%llu,\"spurious_total\":%llu,\"timer_loss_total\":%llu,\"reorder_gap_max\":%llu,\"reorder_delay_max_us\":%llu,"
        "\"cwin_max_bytes\":%llu,\"bytes_in_flight_max\":%llu,\"pacing_rate_max\":%llu,\"blocked_connections\":%llu,\"cc\":\"%s\",\"cc_state\":%llu},"
        "\"bdp_seed\":{\"configured\":%llu,\"applied\":%llu,\"rtt_us_max\":%llu,\"cwin_bytes_max\":%llu},"
        "\"closed\":{\"total\":%llu,\"normal\":%llu,\"timeout\":%llu,\"error\":%llu,\"reset\":%llu},"
        "\"event_loop\":{\"iterations\":%llu,\"over_5ms\":%llu,\"over_20ms\":%llu,\"busy_max_us\":%llu,\"wake_late_max_us\":%llu},"
        "\"udp_errors\":{\"rx\":%llu,\"tx\":%llu,\"rxq_overflow\":%llu,\"queue_pressure_dropped\":%llu},"
        "\"pool_recovery\":{\"retired\":%llu,\"suppressed\":%llu,\"quarantined\":%llu,\"mbb_promotions\":%llu},"
        "\"pmtu\":{\"promotions\":%llu,\"fallbacks\":%llu},"
        "\"fec\":{\"observe\":%d,\"active\":%d,\"tx_blocks\":%llu,\"rx_blocks\":%llu,\"recovered\":%llu,\"nack\":%llu,\"retx\":%llu,"
        "\"udp_adaptive_active\":%d,\"udp_source_packets\":%llu,\"udp_repairs_sent\":%llu,\"udp_repairs_received\":%llu,\"udp_recovered\":%llu,"
        "\"nb_yfe2\":%s}}\n",
        role,worker,release,profile,schema,
        (unsigned long long)s->sessions,(unsigned long long)s->sessions_peak,(unsigned long long)s->sessions_limit,
        (unsigned long long)s->pools,(unsigned long long)s->exit_routes,
        (unsigned long long)s->queue_bytes_used,(unsigned long long)s->queue_bytes_limit,
        (unsigned long long)s->ctrl_sessions,(unsigned long long)s->media_sessions,(unsigned long long)s->bulk_sessions,(unsigned long long)s->unknown_sessions,(unsigned long long)s->udp_sessions,
        (unsigned long long)s->tcp_connecting,(unsigned long long)s->tcp_read_paused,(unsigned long long)s->upstream_fc_blocked,
        (unsigned long long)s->target_connect_failed,
        (unsigned long long)s->lifetime.dns_requests,(unsigned long long)s->lifetime.dns_failures,
        (unsigned long long)s->lifetime.dns_private_rejected,(unsigned long long)s->lifetime.dns_latency_max_us,
        (unsigned long long)s->lifetime.target_connect_timeouts,(unsigned long long)s->lifetime.target_connect_latency_max_us,
        (unsigned long long)s->first_c2s_wait_max_us,(unsigned long long)s->first_s2c_wait_max_us,
        (unsigned long long)s->bytes_c2s,(unsigned long long)s->bytes_s2c,
        (unsigned long long)s->queue_down_bytes,(unsigned long long)s->queue_up_bytes,(unsigned long long)s->queue_q2t_bytes,
        (unsigned long long)s->queue_down_age_max_us,(unsigned long long)s->queue_up_age_max_us,(unsigned long long)s->queue_q2t_age_max_us,
        (unsigned long long)s->link_samples,(unsigned long long)s->link_sent_packets,s->link_effective_loss_max_pct,
        (unsigned long long)s->link_rtt_max_us,(unsigned long long)s->link_jitter_max_us,
        (unsigned long long)s->link_lost_total,(unsigned long long)s->link_spurious_total,(unsigned long long)s->link_timer_loss_total,
        (unsigned long long)s->link_reorder_gap_max,(unsigned long long)s->link_reorder_delay_max_us,
        (unsigned long long)s->link_cwin_max_bytes,(unsigned long long)s->link_bytes_in_flight_max,
        (unsigned long long)s->link_pacing_rate_max,(unsigned long long)s->link_blocked_connections,
        s->link_cc,(unsigned long long)s->link_cc_state,
        (unsigned long long)s->bdp_seed_configured,(unsigned long long)s->bdp_seed_applied,
        (unsigned long long)s->bdp_seed_rtt_max_us,(unsigned long long)s->bdp_seed_cwin_max_bytes,
        (unsigned long long)s->lifetime.close_total,(unsigned long long)s->lifetime.close_normal,(unsigned long long)s->lifetime.close_timeout,(unsigned long long)s->lifetime.close_error,(unsigned long long)s->lifetime.close_reset,
        (unsigned long long)s->lifetime.loop_iterations,(unsigned long long)s->lifetime.loop_over_5ms,(unsigned long long)s->lifetime.loop_over_20ms,
        (unsigned long long)s->lifetime.loop_busy_max_us,(unsigned long long)s->lifetime.loop_wake_late_max_us,
        (unsigned long long)s->lifetime.udp_rx_errors,(unsigned long long)s->lifetime.udp_tx_errors,
        (unsigned long long)s->lifetime.udp_rxq_overflow,(unsigned long long)s->lifetime.udp_queue_pressure_dropped,
        (unsigned long long)s->lifetime.pool_retire_total,(unsigned long long)s->lifetime.pool_retire_suppressed,
        (unsigned long long)s->lifetime.pool_quarantine_total,(unsigned long long)s->lifetime.pool_mbb_promotions,
        (unsigned long long)s->lifetime.pmtu_promotions,(unsigned long long)s->lifetime.pmtu_fallbacks,
        f->observe,f->active,(unsigned long long)f->tx_blocks,(unsigned long long)f->rx_blocks,
        (unsigned long long)f->recovered,(unsigned long long)f->nack,(unsigned long long)f->retx,
        f->udp_adaptive_active,(unsigned long long)f->udp_source_packets,
        (unsigned long long)f->udp_repairs_sent,(unsigned long long)f->udp_repairs_received,
        (unsigned long long)f->udp_recovered,yfe2);
}
