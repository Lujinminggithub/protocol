#ifndef XGW_ROUTE_H
#define XGW_ROUTE_H

/* 多线路运行态模型：配置描述线路，runtime 只消费当前 active/candidate/draining 状态。 */

#include "xgw_protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum xgw_line_state {
    XGW_LINE_IDLE = 0,
    XGW_LINE_ACTIVE,
    XGW_LINE_CANDIDATE,
    XGW_LINE_DRAINING
} xgw_line_state_t;

typedef struct xgw_node_hops {
    const xgw_endpoint_t *current;
    const xgw_endpoint_t *previous;
    const xgw_endpoint_t *next;
    xgw_role_t local_role;
} xgw_node_hops_t;

typedef struct xgw_line_config {
    char id[XGW_MAX_NAME_LEN];
    xgw_fixed_path_t path;
    int priority;
    int enabled;
} xgw_line_config_t;

typedef struct xgw_line_table_config {
    xgw_line_config_t lines[XGW_MAX_LINES];
    size_t line_count;
    char default_line[XGW_MAX_NAME_LEN];
    char route_control_path[160];
    char metrics_path[160];
    uint32_t drain_timeout_sec;
} xgw_line_table_config_t;

typedef struct xgw_line_runtime {
    const xgw_line_config_t *config;
    xgw_node_hops_t hops;
    xgw_line_state_t state;
    time_t state_since;
    time_t drain_until;
} xgw_line_runtime_t;

typedef struct xgw_route_manager {
    xgw_line_runtime_t lines[XGW_MAX_LINES];
    size_t line_count;
    int active_index;
    int candidate_index;
    int draining_index;
    char requested_active_line[XGW_MAX_NAME_LEN];
    char last_command[256];
    time_t last_control_mtime;
    uint32_t drain_timeout_sec;
} xgw_route_manager_t;

const char *xgw_line_state_name(xgw_line_state_t state);
int xgw_route_resolve_hops(const xgw_fixed_path_t *path,
                           xgw_role_t role,
                           const char *hop_name,
                           xgw_node_hops_t *hops);
int xgw_route_manager_init(xgw_route_manager_t *manager,
                           const xgw_line_table_config_t *lines,
                           const xgw_fixed_path_t *fallback_path,
                           xgw_role_t role,
                           const char *hop_name);
const xgw_line_runtime_t *xgw_route_active(const xgw_route_manager_t *manager);
const xgw_line_runtime_t *xgw_route_candidate(const xgw_route_manager_t *manager);
const xgw_line_runtime_t *xgw_route_draining(const xgw_route_manager_t *manager);
xgw_line_runtime_t *xgw_route_find_mut(xgw_route_manager_t *manager, const char *line_id);
const xgw_line_runtime_t *xgw_route_find(const xgw_route_manager_t *manager, const char *line_id);
const xgw_line_runtime_t *xgw_route_find_by_peer(const xgw_route_manager_t *manager,
                                                 const char *host,
                                                 uint16_t port);
int xgw_route_endpoint_address(const xgw_node_hops_t *hops,
                               const xgw_endpoint_t *endpoint,
                               char *out,
                               size_t out_len);
int xgw_route_prewarm(xgw_route_manager_t *manager, const char *line_id, time_t now);
int xgw_route_set_active(xgw_route_manager_t *manager, const char *line_id, time_t now);
int xgw_route_request_active(xgw_route_manager_t *manager, const char *line_id, time_t now);
int xgw_route_commit_requested(xgw_route_manager_t *manager, time_t now);
int xgw_route_drain(xgw_route_manager_t *manager, const char *line_id, time_t now);
void xgw_route_tick(xgw_route_manager_t *manager, time_t now);
int xgw_route_apply_command(xgw_route_manager_t *manager, const char *command, time_t now);
int xgw_route_poll_control_file(xgw_route_manager_t *manager, const char *path, time_t now);
int xgw_route_write_metrics(const xgw_route_manager_t *manager,
                            const char *path,
                            size_t session_count,
                            uint32_t available_mbps);

#endif
