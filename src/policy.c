/* 白名单、黑名单和 DOS 防护实现。 */

#include "xgw_policy.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static int equals_ignore_case(const char *a, const char *b) {
    unsigned char ca;
    unsigned char cb;
    while (*a != '\0' && *b != '\0') {
        ca = (unsigned char) tolower((unsigned char) *a);
        cb = (unsigned char) tolower((unsigned char) *b);
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_ipv4(const char *text, uint32_t *out) {
    struct in_addr addr;
    if (inet_pton(AF_INET, text, &addr) != 1) {
        return 0;
    }
    *out = ntohl(addr.s_addr);
    return 1;
}

static int ends_with_ignore_case(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) {
        return 0;
    }
    return equals_ignore_case(text + text_len - suffix_len, suffix);
}

static int parse_prefix_len(const char *text, int *prefix_len) {
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 0 || parsed > 32) {
        return 0;
    }
    *prefix_len = (int) parsed;
    return 1;
}

static int find_dynamic_ip(const xgw_allow_policy_t *policy, const char *ip) {
    size_t i;
    for (i = 0; i < policy->dynamic_ipv4_count; ++i) {
        if (strcmp(policy->dynamic_ipv4[i].ip, ip) == 0) {
            return (int) i;
        }
    }
    return -1;
}

static int find_counter(const xgw_dos_protector_t *protector, const char *ip) {
    size_t i;
    for (i = 0; i < protector->counter_count; ++i) {
        if (strcmp(protector->counters[i].ip, ip) == 0) {
            return (int) i;
        }
    }
    return -1;
}

static int find_blacklist(const xgw_dos_protector_t *protector, const char *ip) {
    size_t i;
    for (i = 0; i < protector->blacklist_count; ++i) {
        if (strcmp(protector->blacklist[i].ip, ip) == 0) {
            return (int) i;
        }
    }
    return -1;
}

static int ip_in_whitelist(const xgw_dos_protector_t *protector, const char *ip) {
    size_t i;
    for (i = 0; i < protector->config.whitelist_ip_count; ++i) {
        if (strcmp(protector->config.whitelist_ips[i], ip) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_blacklist(xgw_dos_protector_t *protector, const char *ip, const char *reason, time_t now) {
    int idx = find_blacklist(protector, ip);
    if (idx < 0) {
        if (protector->blacklist_count >= XGW_MAX_POLICY_ITEMS) {
            return;
        }
        idx = (int) protector->blacklist_count++;
    }
    copy_text(protector->blacklist[idx].ip, sizeof(protector->blacklist[idx].ip), ip);
    copy_text(protector->blacklist[idx].reason, sizeof(protector->blacklist[idx].reason), reason);
    protector->blacklist[idx].expires_at = now + protector->config.blacklist_duration_sec;
}

void xgw_allow_policy_init(xgw_allow_policy_t *policy, const char *group_id) {
    memset(policy, 0, sizeof(*policy));
    copy_text(policy->group_id, sizeof(policy->group_id), group_id);
}

/* 添加一个 IPv4 CIDR 白名单。 */
int xgw_allow_policy_add_cidr(xgw_allow_policy_t *policy, const char *cidr_text) {
    char buffer[XGW_MAX_IP_LEN];
    char *slash;
    uint32_t ip = 0;
    int prefix_len = 32;
    uint32_t mask = 0xffffffffU;

    if (policy->ip_cidr_count >= XGW_MAX_POLICY_ITEMS) {
        return 0;
    }
    copy_text(buffer, sizeof(buffer), cidr_text);
    slash = strchr(buffer, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (!parse_prefix_len(slash + 1, &prefix_len)) {
            return 0;
        }
    }
    if (!parse_ipv4(buffer, &ip)) {
        return 0;
    }
    if (prefix_len == 0) {
        mask = 0;
    } else if (prefix_len < 32) {
        mask = 0xffffffffU << (32 - prefix_len);
    }
    policy->ip_cidrs[policy->ip_cidr_count].network = ip & mask;
    policy->ip_cidrs[policy->ip_cidr_count].mask = mask;
    ++policy->ip_cidr_count;
    return 1;
}

int xgw_allow_policy_add_exact_domain(xgw_allow_policy_t *policy, const char *domain) {
    if (policy->exact_domain_count >= XGW_MAX_POLICY_ITEMS) {
        return 0;
    }
    copy_text(policy->exact_domains[policy->exact_domain_count], XGW_MAX_DOMAIN_LEN, domain);
    ++policy->exact_domain_count;
    return 1;
}

int xgw_allow_policy_add_suffix_domain(xgw_allow_policy_t *policy, const char *domain) {
    if (policy->suffix_domain_count >= XGW_MAX_POLICY_ITEMS) {
        return 0;
    }
    copy_text(policy->suffix_domains[policy->suffix_domain_count], XGW_MAX_DOMAIN_LEN, domain);
    ++policy->suffix_domain_count;
    return 1;
}

int xgw_allow_policy_allow_ip(const xgw_allow_policy_t *policy, const char *ip_text, time_t now) {
    uint32_t ip = 0;
    size_t i;

    if (!parse_ipv4(ip_text, &ip)) {
        return 0;
    }
    for (i = 0; i < policy->dynamic_ipv4_count; ++i) {
        if (strcmp(policy->dynamic_ipv4[i].ip, ip_text) == 0 &&
            policy->dynamic_ipv4[i].expires_at > now) {
            return 1;
        }
    }
    for (i = 0; i < policy->ip_cidr_count; ++i) {
        if ((ip & policy->ip_cidrs[i].mask) == policy->ip_cidrs[i].network) {
            return 1;
        }
    }
    return 0;
}

/* 检查域名是否符合当前 allow policy。 */
int xgw_allow_policy_match_domain(const xgw_allow_policy_t *policy, const char *domain) {
    size_t i;
    for (i = 0; i < policy->exact_domain_count; ++i) {
        if (equals_ignore_case(policy->exact_domains[i], domain)) {
            return 1;
        }
    }
    for (i = 0; i < policy->suffix_domain_count; ++i) {
        if (ends_with_ignore_case(domain, policy->suffix_domains[i])) {
            return 1;
        }
    }
    return policy->conservative;
}

int xgw_allow_policy_learn_ip(xgw_allow_policy_t *policy, const char *ip_text, uint32_t ttl_sec, time_t now) {
    int idx = find_dynamic_ip(policy, ip_text);
    time_t expires_at = now + ttl_sec + policy->grace_period_sec;

    if (idx < 0) {
        if (policy->dynamic_ipv4_count >= XGW_MAX_POLICY_ITEMS) {
            return 0;
        }
        idx = (int) policy->dynamic_ipv4_count++;
    }
    copy_text(policy->dynamic_ipv4[idx].ip, sizeof(policy->dynamic_ipv4[idx].ip), ip_text);
    policy->dynamic_ipv4[idx].expires_at = expires_at;
    return 1;
}

void xgw_allow_policy_reap(xgw_allow_policy_t *policy, time_t now) {
    size_t i = 0;
    while (i < policy->dynamic_ipv4_count) {
        if (policy->dynamic_ipv4[i].expires_at <= now) {
            policy->dynamic_ipv4[i] = policy->dynamic_ipv4[policy->dynamic_ipv4_count - 1];
            --policy->dynamic_ipv4_count;
            continue;
        }
        ++i;
    }
}

void xgw_dos_init(xgw_dos_protector_t *protector, const xgw_dos_config_t *config) {
    memset(protector, 0, sizeof(*protector));
    protector->config = *config;
}

/* 判断一个来源地址是否通过 DOS 检查。 */
int xgw_dos_allow(xgw_dos_protector_t *protector, const char *ip, time_t now, const char **reason) {
    int idx;

    if (reason != NULL) {
        *reason = "ok";
    }
    if (!protector->config.enabled) {
        return 1;
    }
    if (ip_in_whitelist(protector, ip)) {
        return 1;
    }

    idx = find_blacklist(protector, ip);
    if (idx >= 0 && protector->blacklist[idx].expires_at > now) {
        if (reason != NULL) {
            *reason = protector->blacklist[idx].reason;
        }
        return 0;
    }

    idx = find_counter(protector, ip);
    if (idx < 0) {
        if (protector->counter_count >= XGW_MAX_POLICY_ITEMS) {
            if (reason != NULL) {
                *reason = "counter_table_full";
            }
            return 0;
        }
        idx = (int) protector->counter_count++;
        memset(&protector->counters[idx], 0, sizeof(protector->counters[idx]));
        copy_text(protector->counters[idx].ip, sizeof(protector->counters[idx].ip), ip);
    }

    if (protector->counters[idx].window_sec != now) {
        protector->counters[idx].window_sec = now;
        protector->counters[idx].count_in_window = 0;
    }
    ++protector->counters[idx].count_in_window;

    if (protector->config.max_connections_per_ip > 0 &&
        protector->counters[idx].active_connections >= protector->config.max_connections_per_ip) {
        add_blacklist(protector, ip, "connection limit exceeded", now);
        if (reason != NULL) {
            *reason = "connection limit exceeded";
        }
        return 0;
    }

    if (protector->config.rate_limit_per_second > 0 &&
        protector->counters[idx].count_in_window >
            protector->config.rate_limit_per_second + protector->config.rate_limit_burst) {
        add_blacklist(protector, ip, "rate limit exceeded", now);
        if (reason != NULL) {
            *reason = "rate limit exceeded";
        }
        return 0;
    }

    return 1;
}

void xgw_dos_increment_connection(xgw_dos_protector_t *protector, const char *ip) {
    int idx = find_counter(protector, ip);
    if (idx < 0) {
        return;
    }
    ++protector->counters[idx].active_connections;
}

void xgw_dos_decrement_connection(xgw_dos_protector_t *protector, const char *ip) {
    int idx = find_counter(protector, ip);
    if (idx < 0) {
        return;
    }
    if (protector->counters[idx].active_connections > 0) {
        --protector->counters[idx].active_connections;
    }
}

void xgw_dos_reap(xgw_dos_protector_t *protector, time_t now) {
    size_t i = 0;
    while (i < protector->blacklist_count) {
        if (protector->blacklist[i].expires_at <= now) {
            protector->blacklist[i] = protector->blacklist[protector->blacklist_count - 1];
            --protector->blacklist_count;
            continue;
        }
        ++i;
    }
}
