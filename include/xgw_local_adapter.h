#ifndef XGW_LOCAL_ADAPTER_H
#define XGW_LOCAL_ADAPTER_H

/* 本地适配 ABI：
 * 只定义前端接入层与后端核心之间的最小事件契约。
 * 这个 ABI 不负责拥塞控制、FEC、ACK 或调度策略，
 * 这些都属于 xgw native session/runtime/dataplane/scheduler。
 */

#include <stdint.h>

#define XGW_LOCAL_ADAPTER_MAGIC 0x58474231U

#define XGW_LOCAL_ADAPTER_KIND_TCP_OPEN 1U
#define XGW_LOCAL_ADAPTER_KIND_TCP_DATA 2U
#define XGW_LOCAL_ADAPTER_KIND_TCP_CLOSE 3U
#define XGW_LOCAL_ADAPTER_KIND_UDP_OPEN 4U
#define XGW_LOCAL_ADAPTER_KIND_UDP_DATA 5U
#define XGW_LOCAL_ADAPTER_KIND_UDP_CLOSE 6U
#define XGW_LOCAL_ADAPTER_KIND_TCP_HALF_CLOSE 7U

#define XGW_LOCAL_ADAPTER_META_LEN_V1 8U
#define XGW_LOCAL_ADAPTER_META_LEN 12U
#define XGW_LOCAL_ADAPTER_SESSION_META_LEN 100U

typedef struct xgw_local_adapter_flow_meta {
    uint8_t flow_class;
    uint8_t priority;
    uint8_t budget_flags;
    uint8_t preferred_copies;
    uint32_t read_timeout_ms;
    uint32_t idle_after_first_byte_ms;
} xgw_local_adapter_flow_meta_t;

typedef struct xgw_local_adapter_session_meta {
    char session_id[40];
    char frontend[16];
    char route_name[24];
    char line_id[16];
} xgw_local_adapter_session_meta_t;

#endif
