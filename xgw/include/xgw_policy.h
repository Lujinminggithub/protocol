#ifndef XGW_POLICY_H
#define XGW_POLICY_H

/* 白名单、黑名单以及 DOS 防护相关定义。 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define XGW_MAX_POLICY_ITEMS 128
#define XGW_MAX_IP_LEN 48
#define XGW_MAX_DOMAIN_LEN 128

/* 简化后的 IPv4 CIDR。 */
typedef struct xgw_cidr {
    uint32_t network;
    uint32_t mask;
} xgw_cidr_t;

/* 允许策略对象。 */
typedef struct xgw_allow_policy {
    char group_id[64];
    xgw_cidr_t ip_cidrs[XGW_MAX_POLICY_ITEMS];
    size_t ip_cidr_count;
    char exact_domains[XGW_MAX_POLICY_ITEMS][XGW_MAX_DOMAIN_LEN];
    size_t exact_domain_count;
    char suffix_domains[XGW_MAX_POLICY_ITEMS][XGW_MAX_DOMAIN_LEN];
    size_t suffix_domain_count;
    struct {
        char ip[XGW_MAX_IP_LEN];
        time_t expires_at;
    } dynamic_ipv4[XGW_MAX_POLICY_ITEMS];
    size_t dynamic_ipv4_count;
    int conservative;
    uint32_t grace_period_sec;
} xgw_allow_policy_t;

/* DOS 保护的配置项。 */
typedef struct xgw_dos_config {
    int enabled;
    uint32_t max_connections;
    uint32_t max_connections_per_ip;
    uint32_t rate_limit_per_second;
    uint32_t rate_limit_burst;
    uint32_t blacklist_duration_sec;
    char whitelist_ips[XGW_MAX_POLICY_ITEMS][XGW_MAX_IP_LEN];
    size_t whitelist_ip_count;
} xgw_dos_config_t;

/* 每个 IP 的速率/连接计数。 */
typedef struct xgw_rate_counter {
    char ip[XGW_MAX_IP_LEN];
    time_t window_sec;
    uint32_t count_in_window;
    uint32_t active_connections;
} xgw_rate_counter_t;

/* 黑名单条目。 */
typedef struct xgw_blacklist_entry {
    char ip[XGW_MAX_IP_LEN];
    char reason[96];
    time_t expires_at;
} xgw_blacklist_entry_t;

/* DOS 防护器。 */
typedef struct xgw_dos_protector {
    xgw_dos_config_t config;
    xgw_rate_counter_t counters[XGW_MAX_POLICY_ITEMS];
    size_t counter_count;
    xgw_blacklist_entry_t blacklist[XGW_MAX_POLICY_ITEMS];
    size_t blacklist_count;
} xgw_dos_protector_t;

/* 初始化 allow policy。 */
void xgw_allow_policy_init(xgw_allow_policy_t *policy, const char *group_id);
/* 添加 CIDR 白名单。 */
int xgw_allow_policy_add_cidr(xgw_allow_policy_t *policy, const char *cidr_text);
/* 添加精确域名白名单。 */
int xgw_allow_policy_add_exact_domain(xgw_allow_policy_t *policy, const char *domain);
/* 添加后缀域名白名单。 */
int xgw_allow_policy_add_suffix_domain(xgw_allow_policy_t *policy, const char *domain);
/* 检查目标 IP 是否允许。 */
int xgw_allow_policy_allow_ip(const xgw_allow_policy_t *policy, const char *ip_text, time_t now);
/* 检查域名是否命中策略。 */
int xgw_allow_policy_match_domain(const xgw_allow_policy_t *policy, const char *domain);
/* 动态学习一个 IP。 */
int xgw_allow_policy_learn_ip(xgw_allow_policy_t *policy, const char *ip_text, uint32_t ttl_sec, time_t now);
/* 清理过期动态 IP。 */
void xgw_allow_policy_reap(xgw_allow_policy_t *policy, time_t now);

/* 初始化 DOS 防护器。 */
void xgw_dos_init(xgw_dos_protector_t *protector, const xgw_dos_config_t *config);
/* 检查一个来源地址是否允许通过。 */
int xgw_dos_allow(xgw_dos_protector_t *protector, const char *ip, time_t now, const char **reason);
/* 增加连接计数。 */
void xgw_dos_increment_connection(xgw_dos_protector_t *protector, const char *ip);
/* 减少连接计数。 */
void xgw_dos_decrement_connection(xgw_dos_protector_t *protector, const char *ip);
/* 清理过期黑名单。 */
void xgw_dos_reap(xgw_dos_protector_t *protector, time_t now);

#endif
