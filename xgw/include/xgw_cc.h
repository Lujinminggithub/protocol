#ifndef XGW_CC_H
#define XGW_CC_H

/* 发送侧拥塞控制器：提供 BBR/Brutal/Reno 的统一闭环接口。 */

#include "xgw_protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct xgw_cc_stats {
    uint64_t delivered_bytes;
    uint64_t lost_bytes;
    uint64_t acked_packets;
    uint64_t lost_packets;
    uint64_t latest_rtt_us;
    uint64_t min_rtt_us;
    uint64_t max_bandwidth_bps;
    uint64_t inflight_bytes;
    uint64_t cwnd_bytes;
    uint64_t pacing_rate_bps;
    uint32_t bbr_mode;
} xgw_cc_stats_t;

typedef enum xgw_bbr_mode {
    XGW_BBR_STARTUP = 0,
    XGW_BBR_DRAIN,
    XGW_BBR_PROBE_BW,
    XGW_BBR_PROBE_RTT
} xgw_bbr_mode_t;

typedef struct xgw_cc {
    xgw_congestion_mode_t mode;
    xgw_bbr_profile_t bbr_profile;
    uint64_t target_bandwidth_bps;
    uint64_t pacing_rate_bps;
    uint64_t cwnd_bytes;
    uint64_t inflight_bytes;
    uint64_t delivered_bytes;
    uint64_t lost_bytes;
    uint64_t ack_epoch_bytes;
    uint64_t latest_rtt_us;
    uint64_t min_rtt_us;
    uint64_t srtt_us;
    uint64_t rttvar_us;
    uint64_t max_bandwidth_bps;
    uint64_t bbr_bandwidth_bps;
    uint64_t full_bw_bps;
    uint64_t min_rtt_stamp_us;
    uint64_t next_send_time_ns;
    uint64_t next_round_delivered;
    uint64_t recovery_end_seq;
    uint64_t ssthresh_bytes;
    uint32_t mtu_bytes;
    uint64_t last_ack_ns;
    uint64_t last_send_ns;
    uint64_t acked_packets;
    uint64_t lost_packets;
    uint32_t ack_count;
    uint32_t loss_count;
    uint32_t round_count;
    uint32_t gain_cycle;
    uint32_t full_bw_rounds;
    uint32_t recovery_rounds_left;
    xgw_bbr_mode_t bbr_mode;
    uint8_t app_limited;
    uint8_t round_start;
    double pacing_gain;
    double cwnd_gain;
} xgw_cc_t;

void xgw_cc_init(xgw_cc_t *cc,
                 xgw_congestion_mode_t mode,
                 xgw_bbr_profile_t bbr_profile,
                 uint64_t advertised_tx_bps,
                 uint64_t profile_pacing_bps,
                 uint32_t mtu_bytes);

void xgw_cc_on_send(xgw_cc_t *cc, size_t bytes, uint64_t now_ns);
void xgw_cc_on_ack(xgw_cc_t *cc, size_t bytes, uint64_t rtt_us, uint64_t now_ns);
void xgw_cc_on_loss(xgw_cc_t *cc, size_t bytes, uint64_t now_ns);
void xgw_cc_on_timeout(xgw_cc_t *cc, uint64_t now_ns);

const char *xgw_bbr_mode_name(xgw_bbr_mode_t mode);
uint64_t xgw_cc_pacing_delay_ns(const xgw_cc_t *cc, size_t bytes);
uint64_t xgw_cc_target_inflight(const xgw_cc_t *cc);
int xgw_cc_can_send(const xgw_cc_t *cc, size_t bytes);
/* per-stream 窗口门控:stream 不超自己配额 且 session 总在途不超 cwnd 才放行(消除 HOL)。 */
int xgw_cc_stream_can_send(const xgw_cc_t *cc,
                           uint64_t stream_inflight,
                           uint64_t stream_quota,
                           size_t bytes);
uint64_t xgw_cc_next_send_delay_ns(const xgw_cc_t *cc, size_t bytes, uint64_t now_ns);
void xgw_cc_snapshot(const xgw_cc_t *cc, xgw_cc_stats_t *stats);

#endif
