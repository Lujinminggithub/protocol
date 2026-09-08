#ifndef NB_METRICS_H
#define NB_METRICS_H

#include <stddef.h>
#include <stdint.h>

#include "nb_yfe2_metrics.h"

typedef struct {
    uint64_t bytes_c2s;
    uint64_t bytes_s2c;
    uint64_t close_total;
    uint64_t close_normal;
    uint64_t close_timeout;
    uint64_t close_error;
    uint64_t close_reset;
    uint64_t loop_iterations;
    uint64_t loop_over_5ms;
    uint64_t loop_over_20ms;
    uint64_t loop_busy_max_us;
    uint64_t loop_wake_late_max_us;
    uint64_t udp_rx_errors;
    uint64_t udp_tx_errors;
    uint64_t udp_rxq_overflow;
    uint64_t udp_queue_pressure_dropped;
    uint64_t pool_retire_total;
    uint64_t pool_retire_suppressed;
    uint64_t pool_quarantine_total;
    uint64_t pool_mbb_promotions;
    uint64_t pmtu_promotions;
    uint64_t pmtu_fallbacks;
    uint64_t dns_requests;
    uint64_t dns_failures;
    uint64_t dns_private_rejected;
    uint64_t dns_latency_max_us;
    uint64_t target_connect_timeouts;
    uint64_t target_connect_latency_max_us;
} nb_metrics_state_t;

typedef struct {
    uint64_t sessions;
    uint64_t sessions_peak;
    uint64_t sessions_limit;
    uint64_t queue_bytes_used;
    uint64_t queue_bytes_limit;
    uint64_t pools;
    uint64_t exit_routes;
    uint64_t ctrl_sessions;
    uint64_t media_sessions;
    uint64_t bulk_sessions;
    uint64_t unknown_sessions;
    uint64_t udp_sessions;
    uint64_t tcp_connecting;
    uint64_t tcp_read_paused;
    uint64_t upstream_fc_blocked;
    uint64_t target_connect_failed;
    uint64_t first_c2s_wait_max_us;
    uint64_t first_s2c_wait_max_us;
    uint64_t bytes_c2s;
    uint64_t bytes_s2c;
    uint64_t queue_down_bytes;
    uint64_t queue_up_bytes;
    uint64_t queue_q2t_bytes;
    uint64_t queue_down_age_max_us;
    uint64_t queue_up_age_max_us;
    uint64_t queue_q2t_age_max_us;
    uint64_t link_samples;
    uint64_t link_sent_packets;
    double link_effective_loss_max_pct;
    uint64_t link_rtt_max_us;
    uint64_t link_jitter_max_us;
    uint64_t link_lost_total;
    uint64_t link_spurious_total;
    uint64_t link_timer_loss_total;
    uint64_t link_reorder_gap_max;
    uint64_t link_reorder_delay_max_us;
    uint64_t link_cwin_max_bytes;
    uint64_t link_bytes_in_flight_max;
    uint64_t link_pacing_rate_max;
    uint64_t link_blocked_connections;
    char link_cc[16];
    uint64_t link_cc_state;
    uint64_t bdp_seed_configured;
    uint64_t bdp_seed_applied;
    uint64_t bdp_seed_rtt_max_us;
    uint64_t bdp_seed_cwin_max_bytes;
    nb_metrics_state_t lifetime;
} nb_metrics_snapshot_t;

typedef struct {
    int observe;
    int active;
    uint64_t tx_blocks;
    uint64_t rx_blocks;
    uint64_t recovered;
    uint64_t nack;
    uint64_t retx;
    int udp_adaptive_active;
    uint64_t udp_source_packets;
    uint64_t udp_repairs_sent;
    uint64_t udp_repairs_received;
    uint64_t udp_recovered;
    nb_yfe2_metrics_t nb_yfe2;
} nb_metrics_fec_t;

void nb_metrics_note_close(nb_metrics_state_t* state, const char* reason);
void nb_metrics_note_traffic(nb_metrics_state_t* state, uint64_t bytes_c2s,
    uint64_t bytes_s2c);
void nb_metrics_note_loop(nb_metrics_state_t* state, uint64_t busy_us, uint64_t wake_late_us);
void nb_metrics_note_udp_error(nb_metrics_state_t* state, int transmit);
void nb_metrics_note_udp_rxq_overflow(nb_metrics_state_t* state, uint64_t dropped);
void nb_metrics_note_udp_queue_pressure_drop(nb_metrics_state_t* state,uint64_t dropped);
/* 记录一次 DNS 查询及其失败、私网拒绝和耗时结果。 */
void nb_metrics_note_dns(nb_metrics_state_t* state, int failed, int private_rejected,
    uint64_t latency_us);
/* 记录一次目标建连结果及耗时。 */
void nb_metrics_note_target_connect(nb_metrics_state_t* state, int timeout,
    uint64_t latency_us);
void nb_metrics_snapshot_init(nb_metrics_snapshot_t* snapshot, const nb_metrics_state_t* state,
    uint64_t sessions, uint64_t sessions_peak, uint64_t pools, uint64_t exit_routes);
void nb_metrics_snapshot_add_stream(nb_metrics_snapshot_t* snapshot, int flow_class, int udp_mode,
    int tcp_connecting, int tcp_read_paused, int upstream_fc_blocked, int target_connect_failed,
    uint64_t bytes_c2s, uint64_t bytes_s2c,
    size_t queue_down, size_t queue_up, size_t queue_q2t,
    uint64_t age_down_us, uint64_t age_up_us, uint64_t age_q2t_us,
    uint64_t first_c2s_wait_us, uint64_t first_s2c_wait_us);
void nb_metrics_snapshot_add_link(nb_metrics_snapshot_t* snapshot, double effective_loss_pct,
    uint64_t rtt_us, uint64_t jitter_us, uint64_t sent_packets);
void nb_metrics_snapshot_add_transport(nb_metrics_snapshot_t* snapshot,const char* cc,uint64_t cc_state,
    uint64_t lost,uint64_t spurious,uint64_t timer_loss,uint64_t reorder_gap,uint64_t reorder_delay_us,
    uint64_t cwin,uint64_t bytes_in_flight,uint64_t pacing_rate,int blocked);
void nb_metrics_snapshot_add_seed(nb_metrics_snapshot_t* snapshot,int applied,
    uint64_t rtt_us,uint64_t cwin_bytes);
int nb_metrics_render_json(char* out, size_t cap, const char* role, const char* worker,
    const char* release, const char* profile, int profile_schema,
    const nb_metrics_snapshot_t* snapshot, const nb_metrics_fec_t* fec);

#endif
