#ifndef XGW_TUNING_H
#define XGW_TUNING_H

/* 调优与调试开关，参考 HY2 的窗口、buffer、调试参数入口。 */

#include <stdint.h>

typedef struct xgw_tuning {
    uint32_t udp_rcvbuf_bytes;
    uint32_t udp_sndbuf_bytes;
    uint32_t log_level;
    int enable_debug_timing;
    int enable_summary_dump;
    char summary_path[160];
    uint64_t traffic_baseline_bps;
    uint64_t traffic_burst_bps;
    uint32_t traffic_burst_seconds;
    /* egress 回程出队（burst drain）预算，避免每 tick 只发 1 chunk 的节流。
     * pending_chunks 是软上限（受编译期 XGW_BRIDGE_PENDING_CHUNKS 硬上限钳制）。 */
    uint32_t bridge_egress_pending_chunks;
    uint32_t bridge_egress_dequeue_max_chunks_per_tick;
    uint32_t bridge_egress_dequeue_max_bytes_per_tick;
    uint32_t bridge_egress_dequeue_max_us_per_tick;
} xgw_tuning_t;

void xgw_tuning_init_default(xgw_tuning_t *tuning);

#endif
