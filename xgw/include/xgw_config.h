#ifndef XGW_CONFIG_H
#define XGW_CONFIG_H

/* 运行时配置模型，负责把 key=value 配置文件映射为数据面参数。 */

#include "xgw_acl.h"
#include "xgw_pool.h"
#include "xgw_policy.h"
#include "xgw_protocol.h"
#include "xgw_route.h"
#include "xgw_tuning.h"

/* 单节点运行配置。 */
typedef struct xgw_runtime_config {
    char node_name[XGW_MAX_NAME_LEN];
    xgw_role_t role;
    char transport[16];
    char mtu_profile[32];
    char payload_profile[32];
    char proxy_mode[32];
    char connect_type[32];
    char acl_mode[32];
    char outbound_type[32];
    char outbound_username[64];
    char outbound_password[64];
    char pool_select[32];
    char hop_name[XGW_MAX_NAME_LEN];
    char tun_name[XGW_MAX_NAME_LEN];
    char tun_addr[XGW_MAX_NAME_LEN];
    char bridge_transport[32];
    char bridge_tcp_listen[XGW_MAX_NAME_LEN];
    char bridge_unix_listen[XGW_MAX_NAME_LEN];
    char bridge_ring_path[160];
    char listen_host[XGW_MAX_NAME_LEN];
    char device[XGW_MAX_NAME_LEN];
    uint32_t queue_id;
    xgw_profile_t profile;
    xgw_congestion_mode_t congestion_mode;
    xgw_bbr_profile_t bbr_profile;
    uint64_t advertised_rx_bps;
    uint64_t advertised_tx_bps;
    uint32_t initial_stream_receive_window;
    uint32_t max_stream_receive_window;
    uint32_t initial_connection_receive_window;
    uint32_t max_connection_receive_window;
    uint32_t max_idle_timeout_sec;
    uint32_t keepalive_sec;
    int disable_path_mtu_discovery;
    int enable_udp;
    char auth_token[128];
    xgw_fixed_path_t path;
    xgw_line_table_config_t lines;
    xgw_allow_policy_t allow_policy;
    xgw_dos_config_t dos;
    char manifest_url[160];
    char dns_listen[XGW_MAX_NAME_LEN];
    char dns_upstream[XGW_MAX_NAME_LEN];
    char outbound_host[XGW_MAX_NAME_LEN];
    uint16_t outbound_port;
    char obfs_mode[32];
    char obfs_scope[32];
    char obfs_key[128];
    xgw_acl_engine_t acl;
    xgw_pool_t pool;
    xgw_tuning_t tuning;
} xgw_runtime_config_t;

/* 加载并校验配置文件。 */
int xgw_config_load(const char *path, xgw_runtime_config_t *config, char *error, size_t error_len);
/* 打印配置展开结果，便于调试。 */
void xgw_config_print(const xgw_runtime_config_t *config);

#endif
