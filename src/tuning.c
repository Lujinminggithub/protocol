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
}
