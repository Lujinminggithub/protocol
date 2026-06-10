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
} xgw_tuning_t;

void xgw_tuning_init_default(xgw_tuning_t *tuning);

#endif
