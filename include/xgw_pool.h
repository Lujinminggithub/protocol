#ifndef XGW_POOL_H
#define XGW_POOL_H

/* 资源池与自动选路骨架。 */

#include <stddef.h>
#include <stdint.h>

#define XGW_MAX_POOL_NODES 32

typedef struct xgw_pool_node {
    char name[64];
    char outbound_type[32];
    char host[64];
    uint16_t port;
    char username[64];
    char password[64];
    int priority;
    int penalty;
    int health_score;
} xgw_pool_node_t;

typedef struct xgw_pool {
    xgw_pool_node_t nodes[XGW_MAX_POOL_NODES];
    size_t node_count;
} xgw_pool_t;

void xgw_pool_init(xgw_pool_t *pool);
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
const xgw_pool_node_t *xgw_pool_select_best(const xgw_pool_t *pool);

#endif
