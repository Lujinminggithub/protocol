#ifndef XGW_POOL_H
#define XGW_POOL_H

/* 资源池与自动选路：支持多维指标、固定链路与通用代理统一调度。 */

#include <stddef.h>
#include <stdint.h>

#define XGW_MAX_POOL_NODES 32

typedef struct xgw_pool_metrics {
    uint32_t rtt_ms;
    uint32_t jitter_ms;
    uint32_t loss_permille;
    uint32_t bandwidth_mbps;
    uint32_t load_percent;
    uint32_t auth_failures;
    uint32_t recent_successes;
    uint32_t recent_failures;
} xgw_pool_metrics_t;

typedef struct xgw_pool_weights {
    int rtt_weight;
    int jitter_weight;
    int loss_weight;
    int bandwidth_weight;
    int load_weight;
    int health_weight;
    int priority_weight;
    int sticky_bonus;
} xgw_pool_weights_t;

typedef struct xgw_pool_node {
    char name[64];
    char group[64];
    char outbound_type[32];
    char host[64];
    uint16_t port;
    char username[64];
    char password[64];
    int priority;
    int penalty;
    int health_score;
    int fixed_path;
    int supports_proxy;
    int supports_h3;
    int supports_tls_masq;
    int supports_http_fallback;
    xgw_pool_metrics_t metrics;
} xgw_pool_node_t;

typedef struct xgw_pool {
    xgw_pool_node_t nodes[XGW_MAX_POOL_NODES];
    size_t node_count;
    xgw_pool_weights_t weights;
    char last_selected[64];
} xgw_pool_t;

void xgw_pool_init(xgw_pool_t *pool);
void xgw_pool_set_default_weights(xgw_pool_weights_t *weights);
void xgw_pool_set_weights(xgw_pool_t *pool, const xgw_pool_weights_t *weights);
int xgw_pool_add_node(xgw_pool_t *pool,
                      const char *name,
                      const char *outbound_type,
                      const char *host,
                      uint16_t port,
                      const char *username,
                      const char *password,
                      int priority,
                      int penalty,
                      int health_score);
int xgw_pool_add_node_ex(xgw_pool_t *pool, const xgw_pool_node_t *node);
int xgw_pool_update_metrics(xgw_pool_t *pool, const char *name, const xgw_pool_metrics_t *metrics);
int xgw_pool_score_node(const xgw_pool_t *pool, const xgw_pool_node_t *node, int *score_out);
const xgw_pool_node_t *xgw_pool_select_best(const xgw_pool_t *pool);
const xgw_pool_node_t *xgw_pool_select_for_mode(const xgw_pool_t *pool,
                                                const char *proxy_mode,
                                                const char *required_feature);
void xgw_pool_mark_selected(xgw_pool_t *pool, const char *name);

#endif
