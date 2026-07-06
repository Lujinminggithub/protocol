/* 资源池与自动选路实现。 */

#include "xgw_pool.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

void xgw_pool_init(xgw_pool_t *pool) {
    memset(pool, 0, sizeof(*pool));
}

int xgw_pool_add_node(xgw_pool_t *pool,
                      const char *name,
                      const char *outbound_type,
                      const char *host,
                      uint16_t port,
                      const char *username,
                      const char *password,
                      int priority,
                      int penalty,
                      int health_score) {
    xgw_pool_node_t *node;
    if (pool->node_count >= XGW_MAX_POOL_NODES) {
        return 0;
    }
    node = &pool->nodes[pool->node_count++];
    memset(node, 0, sizeof(*node));
    snprintf(node->name, sizeof(node->name), "%s", name == NULL ? "" : name);
    snprintf(node->outbound_type, sizeof(node->outbound_type), "%s", outbound_type == NULL ? "" : outbound_type);
    snprintf(node->host, sizeof(node->host), "%s", host == NULL ? "" : host);
    snprintf(node->username, sizeof(node->username), "%s", username == NULL ? "" : username);
    snprintf(node->password, sizeof(node->password), "%s", password == NULL ? "" : password);
    node->port = port;
    node->priority = priority;
    node->penalty = penalty;
    node->health_score = health_score;
    return 1;
}

const xgw_pool_node_t *xgw_pool_select_best(const xgw_pool_t *pool) {
    const xgw_pool_node_t *best = NULL;
    int best_score = INT_MIN;
    size_t i;
    for (i = 0; i < pool->node_count; ++i) {
        const xgw_pool_node_t *node = &pool->nodes[i];
        int score = node->health_score + node->priority - node->penalty;
        if (best == NULL || score > best_score) {
            best = node;
            best_score = score;
        }
    }
    return best;
}
