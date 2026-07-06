#ifndef XGW_ACL_H
#define XGW_ACL_H

/* ACL / Outbound 抽象，参考 HY2 的 ACL 与 pluggable outbound 思路。 */

#include <stddef.h>
#include <stdint.h>

#define XGW_MAX_ACL_RULES 128
#define XGW_MAX_OUTBOUNDS 16

typedef enum xgw_acl_action {
    XGW_ACL_DIRECT = 0,
    XGW_ACL_REJECT,
    XGW_ACL_DEFAULT,
    XGW_ACL_NAMED_OUTBOUND
} xgw_acl_action_t;

typedef struct xgw_acl_outbound {
    char name[64];
    char type[32];
    char host[64];
    uint16_t port;
} xgw_acl_outbound_t;

typedef struct xgw_acl_rule {
    char pattern[128];
    xgw_acl_action_t action;
    char outbound_name[64];
} xgw_acl_rule_t;

typedef struct xgw_acl_engine {
    xgw_acl_rule_t rules[XGW_MAX_ACL_RULES];
    size_t rule_count;
    xgw_acl_outbound_t outbounds[XGW_MAX_OUTBOUNDS];
    size_t outbound_count;
} xgw_acl_engine_t;

void xgw_acl_init(xgw_acl_engine_t *engine);
int xgw_acl_add_outbound(xgw_acl_engine_t *engine, const char *name, const char *type, const char *host, uint16_t port);
int xgw_acl_add_rule(xgw_acl_engine_t *engine, const char *pattern, xgw_acl_action_t action, const char *outbound_name);
const xgw_acl_outbound_t *xgw_acl_match(const xgw_acl_engine_t *engine, const char *target);

#endif
