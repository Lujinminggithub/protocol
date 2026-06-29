/* 调优与调试默认值。 */

#include "xgw_tuning.h"

#include <string.h>

void xgw_tuning_init_default(xgw_tuning_t *tuning) {
    memset(tuning, 0, sizeof(*tuning));
    tuning->udp_rcvbuf_bytes = 4U * 1024U * 1024U;
    tuning->udp_sndbuf_bytes = 4U * 1024U * 1024U;
    tuning->log_level = 1U;
    tuning->enable_debug_timing = 0;
    tuning->enable_summary_dump = 1;
    tuning->summary_path[0] = '\0';
    tuning->traffic_baseline_bps = 4ULL * 1000ULL * 1000ULL;
    tuning->traffic_burst_bps = 30ULL * 1000ULL * 1000ULL;
    tuning->traffic_burst_seconds = 600U;
    tuning->bridge_egress_pending_chunks = 32U;
    tuning->bridge_egress_dequeue_max_chunks_per_tick = 64U;
    tuning->bridge_egress_dequeue_max_bytes_per_tick = 1U * 1024U * 1024U;
    tuning->bridge_egress_dequeue_max_us_per_tick = 2000U;
}
