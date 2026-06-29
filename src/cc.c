/* 成熟拥塞控制的工程实现：高频 ACK 驱动的 pacing、inflight 与 BBR/Reno/Brutal 闭环。 */

#include "xgw_cc.h"

#include <string.h>

#define XGW_MIN_RTT_WINDOW_US (10ULL * 1000000ULL)
#define XGW_PROBE_RTT_HOLD_US 200000ULL
#define XGW_MIN_PACING_BPS (1ULL * 1000ULL * 1000ULL)

static uint64_t max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static double max_f64(double a, double b) {
    return a > b ? a : b;
}

static uint64_t bw_to_bdp_bytes(uint64_t bandwidth_bps, uint64_t rtt_us) {
    return ((bandwidth_bps / 8ULL) * max_u64(rtt_us, 1000ULL)) / 1000000ULL;
}

const char *xgw_bbr_mode_name(xgw_bbr_mode_t mode) {
    switch (mode) {
        case XGW_BBR_STARTUP: return "startup";
        case XGW_BBR_DRAIN: return "drain";
        case XGW_BBR_PROBE_BW: return "probe_bw";
        case XGW_BBR_PROBE_RTT: return "probe_rtt";
        default: return "unknown";
    }
}

static void bbr_profile_gains(xgw_bbr_profile_t profile,
                              double *startup_gain,
                              double *probe_up_gain,
                              double *probe_down_gain,
                              double *cwnd_gain) {
    /* 当前只保留 standard 一档增益。 */
    (void) profile;
    *startup_gain = 2.20;
    *probe_up_gain = 1.25;
    *probe_down_gain = 0.75;
    *cwnd_gain = 2.00;
}

static void cc_apply_pacing_rate(xgw_cc_t *cc, uint64_t bandwidth_bps, double gain) {
    uint64_t rate;
    if (cc == NULL) {
        return;
    }
    rate = (uint64_t) ((double) max_u64(bandwidth_bps, XGW_MIN_PACING_BPS) * max_f64(gain, 0.10));
    cc->pacing_rate_bps = max_u64(rate, XGW_MIN_PACING_BPS);
}

static void cc_enter_bbr_mode(xgw_cc_t *cc, xgw_bbr_mode_t mode, uint64_t now_ns) {
    if (cc == NULL) {
        return;
    }
    cc->bbr_mode = mode;
    cc->gain_cycle = 0U;
    cc->last_ack_ns = now_ns;
}

static int cc_advance_round(xgw_cc_t *cc) {
    if (cc == NULL) {
        return 0;
    }
    cc->round_start = 0U;
    if (cc->delivered_bytes >= cc->next_round_delivered) {
        cc->round_start = 1U;
        cc->round_count++;
        cc->next_round_delivered = cc->delivered_bytes + max_u64(cc->inflight_bytes, (uint64_t) cc->mtu_bytes);
        if (cc->recovery_rounds_left > 0U) {
            cc->recovery_rounds_left--;
        }
        return 1;
    }
    return 0;
}

static void cc_update_bbr(xgw_cc_t *cc, uint64_t sample_bw, uint64_t now_ns) {
    uint64_t bw;
    uint64_t rtt_us;
    uint64_t bdp;
    double startup_gain;
    double probe_up_gain;
    double probe_down_gain;
    double cwnd_gain;
    static const double steady_cycle[] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

    if (cc == NULL) {
        return;
    }
    bbr_profile_gains(cc->bbr_profile, &startup_gain, &probe_up_gain, &probe_down_gain, &cwnd_gain);
    bw = max_u64(max_u64(cc->bbr_bandwidth_bps, cc->max_bandwidth_bps), sample_bw);
    if (bw == 0U) {
        bw = max_u64(cc->target_bandwidth_bps, 50ULL * 1000ULL * 1000ULL);
    }
    cc->bbr_bandwidth_bps = bw;
    rtt_us = cc->min_rtt_us == 0U ? max_u64(cc->latest_rtt_us, 1000ULL) : cc->min_rtt_us;
    bdp = bw_to_bdp_bytes(bw, rtt_us);

    if (cc->min_rtt_stamp_us != 0U &&
        cc->latest_rtt_us > 0U &&
        now_ns / 1000ULL > cc->min_rtt_stamp_us + XGW_MIN_RTT_WINDOW_US &&
        cc->bbr_mode != XGW_BBR_PROBE_RTT) {
        cc->recovery_end_seq = now_ns + (XGW_PROBE_RTT_HOLD_US * 1000ULL);
        cc_enter_bbr_mode(cc, XGW_BBR_PROBE_RTT, now_ns);
    }

    switch (cc->bbr_mode) {
        case XGW_BBR_STARTUP:
            if (cc->round_start) {
                if (cc->full_bw_bps == 0U || bw >= (uint64_t) ((double) max_u64(cc->full_bw_bps, 1U) * 1.25)) {
                    cc->full_bw_bps = bw;
                    cc->full_bw_rounds = 0U;
                } else {
                    cc->full_bw_rounds++;
                }
            }
            cc_apply_pacing_rate(cc, bw, startup_gain);
            cc->cwnd_bytes = max_u64((uint64_t) ((double) bdp * cwnd_gain), (uint64_t) cc->mtu_bytes * 32ULL);
            if (cc->full_bw_rounds >= 3U || cc->loss_count > 0U) {
                cc_enter_bbr_mode(cc, XGW_BBR_DRAIN, now_ns);
            }
            break;
        case XGW_BBR_DRAIN:
            cc_apply_pacing_rate(cc, bw, probe_down_gain);
            cc->cwnd_bytes = max_u64((uint64_t) ((double) bdp * cwnd_gain), (uint64_t) cc->mtu_bytes * 16ULL);
            if (cc->inflight_bytes <= max_u64(bdp, (uint64_t) cc->mtu_bytes * 8ULL)) {
                cc_enter_bbr_mode(cc, XGW_BBR_PROBE_BW, now_ns);
            }
            break;
        case XGW_BBR_PROBE_RTT:
            cc_apply_pacing_rate(cc, bw, 1.0);
            cc->cwnd_bytes = max_u64((uint64_t) cc->mtu_bytes * 4ULL, 16ULL * 1024ULL);
            if (cc->inflight_bytes <= cc->cwnd_bytes && cc->recovery_end_seq != 0U && now_ns >= cc->recovery_end_seq) {
                cc->min_rtt_stamp_us = now_ns / 1000ULL;
                cc->recovery_end_seq = 0U;
                cc_enter_bbr_mode(cc, XGW_BBR_PROBE_BW, now_ns);
            }
            break;
        case XGW_BBR_PROBE_BW:
        default: {
            double gain = steady_cycle[cc->gain_cycle % (sizeof(steady_cycle) / sizeof(steady_cycle[0]))];
            uint64_t cycle_ns = max_u64(rtt_us, 50000ULL) * 1000ULL;
            if (cc->round_start || cc->last_ack_ns == 0U || now_ns - cc->last_ack_ns >= cycle_ns) {
                cc->gain_cycle = (cc->gain_cycle + 1U) % 8U;
                cc->last_ack_ns = now_ns;
            }
            if (cc->gain_cycle == 0U) {
                gain = probe_up_gain;
            } else if (cc->gain_cycle == 1U) {
                gain = probe_down_gain;
            }
            cc_apply_pacing_rate(cc, bw, gain);
            cc->cwnd_bytes = max_u64((uint64_t) ((double) bdp * cwnd_gain), (uint64_t) cc->mtu_bytes * 16ULL);
            if (cc->recovery_rounds_left > 0U) {
                cc->cwnd_bytes = max_u64(bdp, (uint64_t) cc->mtu_bytes * 8ULL);
                cc_apply_pacing_rate(cc, bw, 1.0);
            }
            break;
        }
    }
}

static void cc_update_brutal(xgw_cc_t *cc, uint64_t sample_bw) {
    uint64_t base;
    uint64_t ref_rtt_us;
    if (cc == NULL) {
        return;
    }
    base = cc->target_bandwidth_bps;
    if (sample_bw > 0U) {
        base = max_u64(base, sample_bw);
    }
    if (base == 0U) {
        base = 100ULL * 1000ULL * 1000ULL;
    }
    ref_rtt_us = max_u64(cc->latest_rtt_us, cc->srtt_us);
    if (ref_rtt_us == 0U) {
        ref_rtt_us = 100000ULL;
    }
    cc->target_bandwidth_bps = base;
    cc->pacing_rate_bps = base;
    cc->cwnd_bytes = max_u64(bw_to_bdp_bytes(base, ref_rtt_us), 256ULL * 1024ULL);
}

void xgw_cc_init(xgw_cc_t *cc,
                 xgw_congestion_mode_t mode,
                 xgw_bbr_profile_t bbr_profile,
                 uint64_t advertised_tx_bps,
                 uint64_t profile_pacing_bps,
                 uint32_t mtu_bytes) {
    uint64_t seed_bw = advertised_tx_bps;
    memset(cc, 0, sizeof(*cc));
    if (seed_bw == 0U) {
        seed_bw = profile_pacing_bps;
    }
    if (seed_bw == 0U) {
        seed_bw = 200ULL * 1000ULL * 1000ULL;
    }
    if (mtu_bytes == 0U) {
        mtu_bytes = 1200U;
    }
    cc->mode = mode;
    cc->bbr_profile = bbr_profile;
    cc->target_bandwidth_bps = seed_bw;
    cc->pacing_rate_bps = seed_bw;
    cc->cwnd_bytes = max_u64((uint64_t) mtu_bytes * 128ULL, 512ULL * 1024ULL);
    cc->ssthresh_bytes = UINT64_MAX;
    cc->mtu_bytes = mtu_bytes;
    cc->min_rtt_us = 0U;
    cc->srtt_us = 100000U;
    cc->rttvar_us = 50000U;
    cc->latest_rtt_us = 100000U;
    cc->max_bandwidth_bps = seed_bw;
    cc->bbr_bandwidth_bps = seed_bw;
    cc->full_bw_bps = seed_bw;
    cc->bbr_mode = XGW_BBR_STARTUP;
    cc->next_round_delivered = (uint64_t) mtu_bytes;
    cc->pacing_gain = 1.0;
    cc->cwnd_gain = 2.0;
}

void xgw_cc_on_send(xgw_cc_t *cc, size_t bytes, uint64_t now_ns) {
    if (cc == NULL || bytes == 0U) {
        return;
    }
    cc->inflight_bytes += (uint64_t) bytes;
    cc->last_send_ns = now_ns;
}

void xgw_cc_on_ack(xgw_cc_t *cc, size_t bytes, uint64_t rtt_us, uint64_t now_ns) {
    uint64_t sample_bw = 0U;
    if (cc == NULL || bytes == 0U) {
        return;
    }
    cc->acked_packets++;
    cc->delivered_bytes += (uint64_t) bytes;
    if (cc->inflight_bytes >= (uint64_t) bytes) {
        cc->inflight_bytes -= (uint64_t) bytes;
    } else {
        cc->inflight_bytes = 0U;
    }
    if (rtt_us > 0U) {
        cc->latest_rtt_us = rtt_us;
        if (cc->min_rtt_us == 0U || rtt_us < cc->min_rtt_us) {
            cc->min_rtt_us = rtt_us;
            cc->min_rtt_stamp_us = now_ns / 1000ULL;
        }
        if (cc->srtt_us == 0U) {
            cc->srtt_us = rtt_us;
            cc->rttvar_us = rtt_us / 2U;
        } else {
            uint64_t diff = cc->srtt_us > rtt_us ? cc->srtt_us - rtt_us : rtt_us - cc->srtt_us;
            cc->rttvar_us = (3U * cc->rttvar_us + diff) / 4U;
            cc->srtt_us = (7U * cc->srtt_us + rtt_us) / 8U;
        }
        sample_bw = ((uint64_t) bytes * 8ULL * 1000000ULL) / max_u64(rtt_us, 1U);
    }
    if (sample_bw > cc->max_bandwidth_bps) {
        cc->max_bandwidth_bps = sample_bw;
    }
    cc_advance_round(cc);

    switch (cc->mode) {
        case XGW_CC_BRUTAL:
            cc_update_brutal(cc, sample_bw);
            break;
        case XGW_CC_BBR:
        default:
            cc_update_bbr(cc, sample_bw, now_ns);
            break;
    }
}

void xgw_cc_on_loss(xgw_cc_t *cc, size_t bytes, uint64_t now_ns) {
    (void) now_ns;
    if (cc == NULL || bytes == 0U) {
        return;
    }
    cc->loss_count++;
    cc->lost_packets++;
    cc->lost_bytes += (uint64_t) bytes;
    if (cc->inflight_bytes >= (uint64_t) bytes) {
        cc->inflight_bytes -= (uint64_t) bytes;
    } else {
        cc->inflight_bytes = 0U;
    }
    switch (cc->mode) {
        case XGW_CC_BRUTAL:
            cc->pacing_rate_bps = max_u64((cc->pacing_rate_bps * 19ULL) / 20ULL, XGW_MIN_PACING_BPS);
            cc->cwnd_bytes = max_u64((cc->cwnd_bytes * 19ULL) / 20ULL, 128ULL * 1024ULL);
            break;
        case XGW_CC_BBR:
        default:
            cc->pacing_rate_bps = max_u64((cc->pacing_rate_bps * 17ULL) / 20ULL, 2ULL * 1000ULL * 1000ULL);
            cc->cwnd_bytes = max_u64((cc->cwnd_bytes * 9ULL) / 10ULL, (uint64_t) cc->mtu_bytes * 8ULL);
            cc->recovery_rounds_left = 2U;
            if (cc->bbr_mode == XGW_BBR_STARTUP) {
                cc_enter_bbr_mode(cc, XGW_BBR_DRAIN, now_ns);
            }
            break;
    }
}

void xgw_cc_on_timeout(xgw_cc_t *cc, uint64_t now_ns) {
    (void) now_ns;
    if (cc == NULL) {
        return;
    }
    cc->pacing_rate_bps = max_u64(cc->pacing_rate_bps / 2ULL, XGW_MIN_PACING_BPS);
    cc->cwnd_bytes = max_u64(cc->cwnd_bytes / 2ULL, (uint64_t) cc->mtu_bytes * 4ULL);
    cc->ssthresh_bytes = max_u64(cc->cwnd_bytes, (uint64_t) cc->mtu_bytes * 8ULL);
    cc->inflight_bytes = 0U;
    cc->next_send_time_ns = 0U;
}

uint64_t xgw_cc_pacing_delay_ns(const xgw_cc_t *cc, size_t bytes) {
    if (cc == NULL || cc->pacing_rate_bps == 0U || bytes == 0U) {
        return 0U;
    }
    return ((uint64_t) bytes * 8ULL * 1000000000ULL) / max_u64(cc->pacing_rate_bps, 1U);
}

uint64_t xgw_cc_target_inflight(const xgw_cc_t *cc) {
    if (cc == NULL) {
        return 0U;
    }
    return cc->cwnd_bytes;
}

int xgw_cc_can_send(const xgw_cc_t *cc, size_t bytes) {
    if (cc == NULL) {
        return 1;
    }
    if (cc->cwnd_bytes == 0U) {
        return 1;
    }
    return cc->inflight_bytes + (uint64_t) bytes <= cc->cwnd_bytes;
}

/* per-stream 窗口门控:用该 stream 自己的 inflight vs 分给它的 cwnd 配额判断,
 * 而非 session 全局 inflight。这样一条 stream 占满自己配额只卡自己,同 slot(共享 cc)
 * 其他 stream 仍可发 → 消除 head-of-line blocking。
 * 放行需同时满足:
 *   1) 该 stream 不超过自己的 cwnd 配额(per-stream 隔离,防单流独占)。
 *   2) session 总在途不超过 cwnd(物理带宽上限,防整体超发丢包)。
 * 配额按 DRR 权重分配(stream_quota = cwnd × 权重/总权重),配额之和≈cwnd → 不浪费带宽。
 * stream_quota=0 表示未知权重,退化为仅总量门控(与旧行为一致)。 */
int xgw_cc_stream_can_send(const xgw_cc_t *cc,
                           uint64_t stream_inflight,
                           uint64_t stream_quota,
                           size_t bytes) {
    if (cc == NULL || cc->cwnd_bytes == 0U) {
        return 1;
    }
    if (stream_quota > 0U && stream_inflight + (uint64_t) bytes > stream_quota) {
        return 0; /* 本 stream 已占满自己配额:只卡自己,不影响同 slot 其他 stream */
    }
    if (cc->inflight_bytes + (uint64_t) bytes > cc->cwnd_bytes) {
        return 0; /* session 总窗口满(物理带宽上限):真拥塞,全退 */
    }
    return 1;
}

uint64_t xgw_cc_next_send_delay_ns(const xgw_cc_t *cc, size_t bytes, uint64_t now_ns) {
    uint64_t target_ns;
    (void) bytes;
    if (cc == NULL) {
        return 0U;
    }
    target_ns = max_u64(cc->next_send_time_ns, now_ns);
    if (target_ns <= now_ns) {
        return 0U;
    }
    return target_ns - now_ns;
}

void xgw_cc_snapshot(const xgw_cc_t *cc, xgw_cc_stats_t *stats) {
    if (cc == NULL || stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->delivered_bytes = cc->delivered_bytes;
    stats->lost_bytes = cc->lost_bytes;
    stats->acked_packets = cc->acked_packets;
    stats->lost_packets = cc->lost_packets;
    stats->latest_rtt_us = cc->latest_rtt_us;
    stats->min_rtt_us = cc->min_rtt_us;
    stats->max_bandwidth_bps = cc->max_bandwidth_bps;
    stats->inflight_bytes = cc->inflight_bytes;
    stats->cwnd_bytes = cc->cwnd_bytes;
    stats->pacing_rate_bps = cc->pacing_rate_bps;
    stats->bbr_mode = (uint32_t) cc->bbr_mode;
}
