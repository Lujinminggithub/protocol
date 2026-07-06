/* ACL / Outbound 实现。 */

#include "xgw_acl.h"

#include <stdio.h>
#include <string.h>

static int contains_text(const char *text, const char *pattern) {
    if (pattern == NULL || pattern[0] == '\0') {
        return 0;
    }
    return strstr(text, pattern) != NULL;
}

void xgw_acl_init(xgw_acl_engine_t *engine) {
    memset(engine, 0, sizeof(*engine));
}

int xgw_acl_add_outbound(xgw_acl_engine_t *engine, const char *name, const char *type, const char *host, uint16_t port) {
    xgw_acl_outbound_t *item;
    if (engine->outbound_count >= XGW_MAX_OUTBOUNDS) {
        return 0;
    }
    item = &engine->outbounds[engine->outbound_count++];
    memset(item, 0, sizeof(*item));
    snprintf(item->name, sizeof(item->name), "%s", name == NULL ? "" : name);
    snprintf(item->type, sizeof(item->type), "%s", type == NULL ? "" : type);
    snprintf(item->host, sizeof(item->host), "%s", host == NULL ? "" : host);
    item->port = port;
    return 1;
}

int xgw_acl_add_rule(xgw_acl_engine_t *engine, const char *pattern, xgw_acl_action_t action, const char *outbound_name) {
    xgw_acl_rule_t *item;
    if (engine->rule_count >= XGW_MAX_ACL_RULES) {
        return 0;
    }
    item = &engine->rules[engine->rule_count++];
    memset(item, 0, sizeof(*item));
    snprintf(item->pattern, sizeof(item->pattern), "%s", pattern == NULL ? "" : pattern);
    item->action = action;
    snprintf(item->outbound_name, sizeof(item->outbound_name), "%s", outbound_name == NULL ? "" : outbound_name);
    return 1;
}

const xgw_acl_outbound_t *xgw_acl_match(const xgw_acl_engine_t *engine, const char *target) {
    size_t i;
    const xgw_acl_outbound_t *default_ob = NULL;
    for (i = 0; i < engine->outbound_count; ++i) {
        if (strcmp(engine->outbounds[i].name, "default") == 0) {
            default_ob = &engine->outbounds[i];
            break;
        }
    }
    for (i = 0; i < engine->rule_count; ++i) {
        const xgw_acl_rule_t *rule = &engine->rules[i];
        size_t j;
        if (!contains_text(target, rule->pattern)) {
            continue;
        }
        if (rule->action == XGW_ACL_REJECT) {
            return NULL;
        }
        if (rule->action == XGW_ACL_DEFAULT || rule->action == XGW_ACL_DIRECT) {
            return default_ob;
        }
        if (rule->action == XGW_ACL_NAMED_OUTBOUND) {
            for (j = 0; j < engine->outbound_count; ++j) {
                if (strcmp(engine->outbounds[j].name, rule->outbound_name) == 0) {
                    return &engine->outbounds[j];
                }
            }
        }
    }
    return default_ob;
}
