/* 配置加载器：把 key=value 文本配置转换为运行时结构体。 */

#include "xgw_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct config_acl_rule_ctx {
    xgw_runtime_config_t *config;
    xgw_acl_action_t action;
    const char *outbound_name;
} config_acl_rule_ctx_t;

typedef struct config_pool_ctx {
    xgw_runtime_config_t *config;
    char name[64];
    char type[32];
    char host[64];
    uint16_t port;
    char username[64];
    char password[64];
    int priority;
    int penalty;
    int health_score;
} config_pool_ctx_t;

static char *trim(char *text) {
    char *end;
    while (*text != '\0' && isspace((unsigned char) *text)) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char) end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static int equals_text(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

/* 统一设置错误信息。 */
static void set_error(char *error, size_t error_len, const char *message, const char *detail) {
    if (error_len == 0) {
        return;
    }
    if (detail != NULL && detail[0] != '\0') {
        snprintf(error, error_len, "%s: %s", message, detail);
    } else {
        snprintf(error, error_len, "%s", message);
    }
}

static int parse_uint32(const char *value, uint32_t *out) {
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        return 0;
    }
    *out = (uint32_t) parsed;
    return 1;
}

static int parse_uint64(const char *value, uint64_t *out) {
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        return 0;
    }
    *out = (uint64_t) parsed;
    return 1;
}

static int parse_bool(const char *value, int *out) {
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0 || strcmp(value, "no") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int parse_uint16(const char *value, uint16_t *out) {
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > 65535UL) {
        return 0;
    }
    *out = (uint16_t) parsed;
    return 1;
}

static void apply_mtu_profile(xgw_runtime_config_t *config, const char *value) {
    snprintf(config->mtu_profile, sizeof(config->mtu_profile), "%s", value);
    if (equals_text(value, "mobile-safe")) {
        config->profile.mtu = 1280;
        if (config->profile.payload_size == 0 || config->profile.payload_size > 1100) {
            config->profile.payload_size = 1100;
        }
        return;
    }
    if (equals_text(value, "relay-balanced")) {
        config->profile.mtu = 1360;
        if (config->profile.payload_size == 0 || config->profile.payload_size > 1180) {
            config->profile.payload_size = 1180;
        }
        return;
    }
    if (equals_text(value, "jumboish")) {
        config->profile.mtu = 1420;
        if (config->profile.payload_size == 0 || config->profile.payload_size > 1240) {
            config->profile.payload_size = 1240;
        }
        return;
    }
    config->profile.mtu = 1380;
}

/* 根据 payload profile 套用不同负载大小偏好。 */
static void apply_payload_profile(xgw_runtime_config_t *config, const char *value) {
    snprintf(config->payload_profile, sizeof(config->payload_profile), "%s", value);
    if (equals_text(value, "safe")) {
        config->profile.payload_size = 1100;
        return;
    }
    if (equals_text(value, "balanced")) {
        config->profile.payload_size = 1180;
        return;
    }
    if (equals_text(value, "aggressive")) {
        config->profile.payload_size = 1240;
        return;
    }
    config->profile.payload_size = 1200;
}

static xgw_role_t parse_role(const char *value) {
    if (equals_text(value, "mobile")) {
        return XGW_ROLE_MOBILE;
    }
    if (equals_text(value, "ingress")) {
        return XGW_ROLE_INGRESS;
    }
    if (equals_text(value, "relay")) {
        return XGW_ROLE_RELAY;
    }
    if (equals_text(value, "egress")) {
        return XGW_ROLE_EGRESS;
    }
    return XGW_ROLE_UNKNOWN;
}

static xgw_profile_mode_t parse_profile(const char *value) {
    if (equals_text(value, "live-bbr")) {
        return XGW_PROFILE_BBR;
    }
    if (equals_text(value, "live-brutal")) {
        return XGW_PROFILE_BRUTAL;
    }
    return XGW_PROFILE_NONE;
}

static void split_csv(const char *value, void (*consumer)(const char *, void *), void *ctx) {
    char buffer[512];
    char *cursor;
    char *comma;

    snprintf(buffer, sizeof(buffer), "%s", value);
    cursor = buffer;
    while (*cursor != '\0') {
        comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        consumer(trim(cursor), ctx);
        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
    }
}

typedef struct config_list_ctx {
    xgw_runtime_config_t *config;
    int is_suffix;
} config_list_ctx_t;

static void add_hop(const char *entry, void *ctx) {
    xgw_runtime_config_t *config = (xgw_runtime_config_t *) ctx;
    xgw_endpoint_t *hop;
    char value[160];
    char *at;
    char *eq;
    if (config->path.hop_count >= XGW_MAX_ENDPOINTS) {
        return;
    }
    snprintf(value, sizeof(value), "%s", entry);
    at = strchr(value, '@');
    eq = strchr(value, '=');
    if (at == NULL || eq == NULL || at > eq) {
        return;
    }
    *at = '\0';
    *eq = '\0';
    hop = &config->path.hops[config->path.hop_count++];
    snprintf(hop->name, sizeof(hop->name), "%s", trim(value));
    hop->role = parse_role(trim(at + 1));
    snprintf(hop->address, sizeof(hop->address), "%s", trim(eq + 1));
    hop->stable = (hop->role == XGW_ROLE_RELAY) ? 1 : 0;
}

static void add_whitelist_ip(const char *entry, void *ctx) {
    xgw_dos_config_t *dos = (xgw_dos_config_t *) ctx;
    if (dos->whitelist_ip_count >= XGW_MAX_POLICY_ITEMS) {
        return;
    }
    snprintf(dos->whitelist_ips[dos->whitelist_ip_count],
             sizeof(dos->whitelist_ips[dos->whitelist_ip_count]),
             "%s",
             entry);
    ++dos->whitelist_ip_count;
}

static void add_domain_item(const char *entry, void *ctx) {
    config_list_ctx_t *list = (config_list_ctx_t *) ctx;
    if (list->is_suffix) {
        xgw_allow_policy_add_suffix_domain(&list->config->allow_policy, entry);
    } else {
        xgw_allow_policy_add_exact_domain(&list->config->allow_policy, entry);
    }
}

static void add_cidr_item(const char *entry, void *ctx) {
    xgw_runtime_config_t *config = (xgw_runtime_config_t *) ctx;
    xgw_allow_policy_add_cidr(&config->allow_policy, entry);
}

static void add_acl_rule_item(const char *entry, void *ctx) {
    config_acl_rule_ctx_t *rule_ctx = (config_acl_rule_ctx_t *) ctx;
    xgw_acl_add_rule(&rule_ctx->config->acl, entry, rule_ctx->action, rule_ctx->outbound_name);
}

static int apply_key_value(xgw_runtime_config_t *config, const char *key, const char *value, char *error, size_t error_len) {
    int flag = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    config_list_ctx_t list_ctx;

    if (equals_text(key, "node_name")) {
        snprintf(config->node_name, sizeof(config->node_name), "%s", value);
        return 1;
    }
    if (equals_text(key, "transport")) {
        snprintf(config->transport, sizeof(config->transport), "%s", value);
        return 1;
    }
    if (equals_text(key, "proxy_mode")) {
        snprintf(config->proxy_mode, sizeof(config->proxy_mode), "%s", value);
        return 1;
    }
    if (equals_text(key, "acl_mode")) {
        snprintf(config->acl_mode, sizeof(config->acl_mode), "%s", value);
        return 1;
    }
    if (equals_text(key, "outbound_type")) {
        snprintf(config->outbound_type, sizeof(config->outbound_type), "%s", value);
        xgw_acl_add_outbound(&config->acl, "default", value, config->outbound_host, config->outbound_port);
        return 1;
    }
    if (equals_text(key, "outbound_username")) {
        snprintf(config->outbound_username, sizeof(config->outbound_username), "%s", value);
        return 1;
    }
    if (equals_text(key, "outbound_password")) {
        snprintf(config->outbound_password, sizeof(config->outbound_password), "%s", value);
        return 1;
    }
    if (equals_text(key, "pool_select")) {
        snprintf(config->pool_select, sizeof(config->pool_select), "%s", value);
        return 1;
    }
    if (equals_text(key, "mtu_profile")) {
        apply_mtu_profile(config, value);
        return 1;
    }
    if (equals_text(key, "payload_profile")) {
        apply_payload_profile(config, value);
        return 1;
    }
    if (equals_text(key, "hop_name")) {
        snprintf(config->hop_name, sizeof(config->hop_name), "%s", value);
        return 1;
    }
    if (equals_text(key, "tun_name")) {
        snprintf(config->tun_name, sizeof(config->tun_name), "%s", value);
        return 1;
    }
    if (equals_text(key, "tun_addr")) {
        snprintf(config->tun_addr, sizeof(config->tun_addr), "%s", value);
        return 1;
    }
    if (equals_text(key, "listen_host")) {
        snprintf(config->listen_host, sizeof(config->listen_host), "%s", value);
        return 1;
    }
    if (equals_text(key, "device")) {
        snprintf(config->device, sizeof(config->device), "%s", value);
        return 1;
    }
    if (equals_text(key, "queue_id") && parse_uint32(value, &u32)) {
        config->queue_id = u32;
        return 1;
    }
    if (equals_text(key, "congestion") || equals_text(key, "congestion_mode")) {
        if (!xgw_parse_congestion_mode(value, &config->congestion_mode)) {
            set_error(error, error_len, "invalid congestion mode", value);
            return 0;
        }
        return 1;
    }
    if (equals_text(key, "bbr_profile")) {
        if (!xgw_parse_bbr_profile(value, &config->bbr_profile)) {
            set_error(error, error_len, "invalid bbr_profile", value);
            return 0;
        }
        return 1;
    }
    if (equals_text(key, "auth_token")) {
        snprintf(config->auth_token, sizeof(config->auth_token), "%s", value);
        return 1;
    }
    if (equals_text(key, "advertised_rx_bps") && parse_uint64(value, &u64)) {
        config->advertised_rx_bps = u64;
        return 1;
    }
    if (equals_text(key, "advertised_tx_bps") && parse_uint64(value, &u64)) {
        config->advertised_tx_bps = u64;
        return 1;
    }
    if (equals_text(key, "initial_stream_receive_window") && parse_uint32(value, &u32)) {
        config->initial_stream_receive_window = u32;
        return 1;
    }
    if (equals_text(key, "max_stream_receive_window") && parse_uint32(value, &u32)) {
        config->max_stream_receive_window = u32;
        return 1;
    }
    if (equals_text(key, "initial_connection_receive_window") && parse_uint32(value, &u32)) {
        config->initial_connection_receive_window = u32;
        return 1;
    }
    if (equals_text(key, "max_connection_receive_window") && parse_uint32(value, &u32)) {
        config->max_connection_receive_window = u32;
        return 1;
    }
    if (equals_text(key, "max_idle_timeout_sec") && parse_uint32(value, &u32)) {
        config->max_idle_timeout_sec = u32;
        return 1;
    }
    if (equals_text(key, "keepalive_sec") && parse_uint32(value, &u32)) {
        config->keepalive_sec = u32;
        return 1;
    }
    if (equals_text(key, "disable_path_mtu_discovery") && parse_bool(value, &flag)) {
        config->disable_path_mtu_discovery = flag;
        return 1;
    }
    if (equals_text(key, "enable_udp") && parse_bool(value, &flag)) {
        config->enable_udp = flag;
        return 1;
    }
    if (equals_text(key, "outbound_host")) {
        snprintf(config->outbound_host, sizeof(config->outbound_host), "%s", value);
        return 1;
    }
    if (equals_text(key, "outbound_port") && parse_uint16(value, &config->outbound_port)) {
        return 1;
    }
    if (equals_text(key, "udp_rcvbuf_bytes") && parse_uint32(value, &u32)) {
        config->tuning.udp_rcvbuf_bytes = u32;
        return 1;
    }
    if (equals_text(key, "udp_sndbuf_bytes") && parse_uint32(value, &u32)) {
        config->tuning.udp_sndbuf_bytes = u32;
        return 1;
    }
    if (equals_text(key, "log_level") && parse_uint32(value, &u32)) {
        config->tuning.log_level = u32;
        return 1;
    }
    if (equals_text(key, "enable_debug_timing") && parse_bool(value, &flag)) {
        config->tuning.enable_debug_timing = flag;
        return 1;
    }
    if (equals_text(key, "enable_summary_dump") && parse_bool(value, &flag)) {
        config->tuning.enable_summary_dump = flag;
        return 1;
    }
    if (equals_text(key, "acl_allow")) {
        config_acl_rule_ctx_t rule_ctx;
        rule_ctx.config = config;
        rule_ctx.action = XGW_ACL_DIRECT;
        rule_ctx.outbound_name = "default";
        split_csv(value, add_acl_rule_item, &rule_ctx);
        return 1;
    }
    if (equals_text(key, "acl_reject")) {
        config_acl_rule_ctx_t rule_ctx;
        rule_ctx.config = config;
        rule_ctx.action = XGW_ACL_REJECT;
        rule_ctx.outbound_name = "";
        split_csv(value, add_acl_rule_item, &rule_ctx);
        return 1;
    }
    if (strncmp(key, "acl_outbound.", 13) == 0) {
        const char *ob_name = key + 13;
        config_acl_rule_ctx_t rule_ctx;
        rule_ctx.config = config;
        rule_ctx.action = XGW_ACL_NAMED_OUTBOUND;
        rule_ctx.outbound_name = ob_name;
        split_csv(value, add_acl_rule_item, &rule_ctx);
        return 1;
    }
    if (strncmp(key, "pool.", 5) == 0) {
        char name[64];
        char type[32];
        char host[64];
        char username[64];
        char password[64];
        int priority = 10;
        int penalty = 0;
        int health_score = 100;
        uint16_t port = 0;
        char spec[256];
        char *cursor;
        snprintf(name, sizeof(name), "%s", key + 5);
        spec[0] = '\0';
        snprintf(spec, sizeof(spec), "%s", value);
        cursor = spec;
        type[0] = '\0';
        host[0] = '\0';
        username[0] = '\0';
        password[0] = '\0';
        while (*cursor != '\0') {
            char *part;
            char *comma = strchr(cursor, ',');
            if (comma != NULL) {
                *comma = '\0';
            }
            part = trim(cursor);
            if (strncmp(part, "type=", 5) == 0) {
                snprintf(type, sizeof(type), "%s", part + 5);
            } else if (strncmp(part, "host=", 5) == 0) {
                snprintf(host, sizeof(host), "%s", part + 5);
            } else if (strncmp(part, "port=", 5) == 0) {
                parse_uint16(part + 5, &port);
            } else if (strncmp(part, "user=", 5) == 0) {
                snprintf(username, sizeof(username), "%s", part + 5);
            } else if (strncmp(part, "pass=", 5) == 0) {
                snprintf(password, sizeof(password), "%s", part + 5);
            } else if (strncmp(part, "priority=", 9) == 0) {
                priority = atoi(part + 9);
            } else if (strncmp(part, "penalty=", 8) == 0) {
                penalty = atoi(part + 8);
            } else if (strncmp(part, "health=", 7) == 0) {
                health_score = atoi(part + 7);
            }
            if (comma == NULL) {
                break;
            }
            cursor = comma + 1;
        }
        xgw_pool_add_node(&config->pool, name, type, host, port, username, password, priority, penalty, health_score);
        xgw_acl_add_outbound(&config->acl, name, type, host, port);
        return 1;
    }
    if (equals_text(key, "obfs_mode")) {
        snprintf(config->obfs_mode, sizeof(config->obfs_mode), "%s", value);
        return 1;
    }
    if (equals_text(key, "obfs_scope")) {
        snprintf(config->obfs_scope, sizeof(config->obfs_scope), "%s", value);
        return 1;
    }
    if (equals_text(key, "obfs_key")) {
        snprintf(config->obfs_key, sizeof(config->obfs_key), "%s", value);
        return 1;
    }
    if (equals_text(key, "role")) {
        config->role = parse_role(value);
        if (config->role == XGW_ROLE_UNKNOWN) {
            set_error(error, error_len, "invalid role", value);
            return 0;
        }
        return 1;
    }
    if (equals_text(key, "profile")) {
        xgw_profile_init_default(&config->profile, parse_profile(value));
        return 1;
    }
    if (equals_text(key, "path")) {
        config->path.hop_count = 0;
        split_csv(value, add_hop, config);
        return config->path.hop_count > 0;
    }
    if (equals_text(key, "mtu") && parse_uint32(value, &u32)) {
        config->profile.mtu = u32;
        return 1;
    }
    if (equals_text(key, "payload_size") && parse_uint32(value, &u32)) {
        config->profile.payload_size = u32;
        return 1;
    }
    if (equals_text(key, "reorder_window") && parse_uint32(value, &u32)) {
        config->profile.reorder_window = u32;
        return 1;
    }
    if (equals_text(key, "fec_data_shards") && parse_uint32(value, &u32)) {
        config->profile.fec_data_shards = u32;
        return 1;
    }
    if (equals_text(key, "fec_parity_shards") && parse_uint32(value, &u32)) {
        config->profile.fec_parity_shards = u32;
        return 1;
    }
    if (equals_text(key, "pacing_rate_bps") && parse_uint64(value, &u64)) {
        config->profile.pacing_rate_bps = u64;
        return 1;
    }
    if (equals_text(key, "pacing_interval_us") && parse_uint32(value, &u32)) {
        config->profile.pacing_interval_us = u32;
        return 1;
    }
    if (equals_text(key, "redundant_copies") && parse_uint32(value, &u32)) {
        config->profile.redundant_copies = u32;
        return 1;
    }
    if (equals_text(key, "manifest_url")) {
        snprintf(config->manifest_url, sizeof(config->manifest_url), "%s", value);
        return 1;
    }
    if (equals_text(key, "dns_listen")) {
        snprintf(config->dns_listen, sizeof(config->dns_listen), "%s", value);
        return 1;
    }
    if (equals_text(key, "dns_upstream")) {
        snprintf(config->dns_upstream, sizeof(config->dns_upstream), "%s", value);
        return 1;
    }
    if (equals_text(key, "policy_group")) {
        snprintf(config->allow_policy.group_id, sizeof(config->allow_policy.group_id), "%s", value);
        return 1;
    }
    if (equals_text(key, "allow_cidrs")) {
        split_csv(value, add_cidr_item, config);
        return 1;
    }
    if (equals_text(key, "allow_domains")) {
        list_ctx.config = config;
        list_ctx.is_suffix = 0;
        split_csv(value, add_domain_item, &list_ctx);
        return 1;
    }
    if (equals_text(key, "allow_domain_suffixes")) {
        list_ctx.config = config;
        list_ctx.is_suffix = 1;
        split_csv(value, add_domain_item, &list_ctx);
        return 1;
    }
    if (equals_text(key, "conservative_domain_allow") && parse_bool(value, &flag)) {
        config->allow_policy.conservative = flag;
        return 1;
    }
    if (equals_text(key, "dynamic_grace_period_sec") && parse_uint32(value, &u32)) {
        config->allow_policy.grace_period_sec = u32;
        return 1;
    }
    if (equals_text(key, "dos_enabled") && parse_bool(value, &flag)) {
        config->dos.enabled = flag;
        return 1;
    }
    if (equals_text(key, "max_connections") && parse_uint32(value, &u32)) {
        config->dos.max_connections = u32;
        return 1;
    }
    if (equals_text(key, "max_connections_per_ip") && parse_uint32(value, &u32)) {
        config->dos.max_connections_per_ip = u32;
        return 1;
    }
    if (equals_text(key, "rate_limit_per_second") && parse_uint32(value, &u32)) {
        config->dos.rate_limit_per_second = u32;
        return 1;
    }
    if (equals_text(key, "rate_limit_burst") && parse_uint32(value, &u32)) {
        config->dos.rate_limit_burst = u32;
        return 1;
    }
    if (equals_text(key, "blacklist_duration_sec") && parse_uint32(value, &u32)) {
        config->dos.blacklist_duration_sec = u32;
        return 1;
    }
    if (equals_text(key, "whitelist_ips")) {
        config->dos.whitelist_ip_count = 0;
        split_csv(value, add_whitelist_ip, &config->dos);
        return 1;
    }

    set_error(error, error_len, "unknown or invalid key", key);
    return 0;
}

/* 加载配置文件并做基础校验。 */
int xgw_config_load(const char *path, xgw_runtime_config_t *config, char *error, size_t error_len) {
    FILE *fp = NULL;
    char line[512];

    memset(config, 0, sizeof(*config));
    xgw_profile_init_default(&config->profile, XGW_PROFILE_BBR);
    xgw_allow_policy_init(&config->allow_policy, "default");
    xgw_acl_init(&config->acl);
    xgw_pool_init(&config->pool);
    xgw_tuning_init_default(&config->tuning);
    snprintf(config->transport, sizeof(config->transport), "%s", "udp");
    snprintf(config->mtu_profile, sizeof(config->mtu_profile), "%s", "default");
    snprintf(config->payload_profile, sizeof(config->payload_profile), "%s", "default");
    snprintf(config->proxy_mode, sizeof(config->proxy_mode), "%s", "fixed-path");
    snprintf(config->acl_mode, sizeof(config->acl_mode), "%s", "allow");
    snprintf(config->outbound_type, sizeof(config->outbound_type), "%s", "direct");
    snprintf(config->pool_select, sizeof(config->pool_select), "%s", "best");
    snprintf(config->obfs_mode, sizeof(config->obfs_mode), "%s", "none");
    snprintf(config->obfs_scope, sizeof(config->obfs_scope), "%s", "hop");
    snprintf(config->listen_host, sizeof(config->listen_host), "%s", "0.0.0.0");
    config->congestion_mode = XGW_CC_BBR;
    config->bbr_profile = XGW_BBR_STANDARD;
    config->advertised_rx_bps = 0;
    config->advertised_tx_bps = 0;
    config->initial_stream_receive_window = 8U * 1024U * 1024U;
    config->max_stream_receive_window = 8U * 1024U * 1024U;
    config->initial_connection_receive_window = 20U * 1024U * 1024U;
    config->max_connection_receive_window = 20U * 1024U * 1024U;
    config->max_idle_timeout_sec = 30U;
    config->keepalive_sec = 10U;
    config->disable_path_mtu_discovery = 0;
    config->enable_udp = 1;

    #ifdef _WIN32
    if (fopen_s(&fp, path, "r") != 0) {
        fp = NULL;
    }
    #else
    fp = fopen(path, "r");
    #endif
    if (fp == NULL) {
        set_error(error, error_len, "cannot open config", path);
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *body = trim(line);
        char *eq = NULL;
        if (body[0] == '\0' || body[0] == '#') {
            continue;
        }
        eq = strchr(body, '=');
        if (eq == NULL) {
            fclose(fp);
            set_error(error, error_len, "expected key=value on line", "");
            return 0;
        }
        *eq = '\0';
        if (!apply_key_value(config, trim(body), trim(eq + 1), error, error_len)) {
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);

    if (config->role == XGW_ROLE_UNKNOWN) {
        set_error(error, error_len, "role is required", "");
        return 0;
    }
    if (config->path.hop_count < 2) {
        set_error(error, error_len, "path requires at least two hops", "");
        return 0;
    }
    if (config->profile.mode != XGW_PROFILE_NONE &&
        config->profile.fec_data_shards > 0 &&
        config->profile.fec_parity_shards == 0 &&
        config->profile.mode != XGW_PROFILE_BBR) {
        set_error(error, error_len, "fec parity shards required", "");
        return 0;
    }
    return 1;
}

/* 输出当前配置展开结果。 */
void xgw_config_print(const xgw_runtime_config_t *config) {
    size_t i;
    printf("node_name=%s\n", config->node_name);
    printf("transport=%s proxy_mode=%s acl_mode=%s outbound_type=%s pool_select=%s mtu_profile=%s payload_profile=%s hop_name=%s tun_name=%s tun_addr=%s listen_host=%s device=%s queue_id=%u\n",
           config->transport,
           config->proxy_mode,
           config->acl_mode,
           config->outbound_type,
           config->pool_select,
           config->mtu_profile,
           config->payload_profile,
           config->hop_name,
           config->tun_name,
           config->tun_addr,
           config->listen_host,
           config->device,
           config->queue_id);
    printf("role=%s\n", xgw_role_name(config->role));
    printf("profile=%s\n", xgw_profile_name(config->profile.mode));
    printf("congestion=%s bbr_profile=%s auth_token=%s enable_udp=%d\n",
           xgw_congestion_mode_name(config->congestion_mode),
           xgw_bbr_profile_name(config->bbr_profile),
           config->auth_token,
           config->enable_udp);
    printf("mtu=%u payload_size=%u reorder_window=%u\n",
           config->profile.mtu,
           config->profile.payload_size,
           config->profile.reorder_window);
    printf("rx_bps=%llu tx_bps=%llu stream_win=%u/%u conn_win=%u/%u idle_timeout=%u keepalive=%u disable_pmtud=%d outbound=%s:%u outbound_auth=%s/%s obfs=%s scope=%s\n",
           (unsigned long long) config->advertised_rx_bps,
           (unsigned long long) config->advertised_tx_bps,
           config->initial_stream_receive_window,
           config->max_stream_receive_window,
           config->initial_connection_receive_window,
           config->max_connection_receive_window,
           config->max_idle_timeout_sec,
           config->keepalive_sec,
           config->disable_path_mtu_discovery,
           config->outbound_host,
           config->outbound_port,
           config->outbound_username,
           config->outbound_password,
           config->obfs_mode,
           config->obfs_scope);
    printf("tuning rcvbuf=%u sndbuf=%u log_level=%u debug_timing=%d summary_dump=%d pool_nodes=%zu acl_rules=%zu\n",
           config->tuning.udp_rcvbuf_bytes,
           config->tuning.udp_sndbuf_bytes,
           config->tuning.log_level,
           config->tuning.enable_debug_timing,
           config->tuning.enable_summary_dump,
           config->pool.node_count,
           config->acl.rule_count);
    printf("fec=%u/%u pacing_rate_bps=%llu pacing_interval_us=%u copies=%u\n",
           config->profile.fec_data_shards,
           config->profile.fec_parity_shards,
           (unsigned long long) config->profile.pacing_rate_bps,
           config->profile.pacing_interval_us,
           config->profile.redundant_copies);
    printf("path:");
    for (i = 0; i < config->path.hop_count; ++i) {
        printf(" %s@%s=%s",
               config->path.hops[i].name,
               xgw_role_name(config->path.hops[i].role),
               config->path.hops[i].address);
        if (i + 1 < config->path.hop_count) {
            printf(",");
        }
    }
    printf("\n");
    printf("policy_group=%s allow_cidrs=%zu exact_domains=%zu suffix_domains=%zu conservative=%d grace=%u\n",
           config->allow_policy.group_id,
           config->allow_policy.ip_cidr_count,
           config->allow_policy.exact_domain_count,
           config->allow_policy.suffix_domain_count,
           config->allow_policy.conservative,
           config->allow_policy.grace_period_sec);
    printf("dos_enabled=%d whitelist_ips=%zu blacklist_duration_sec=%u\n",
           config->dos.enabled,
           config->dos.whitelist_ip_count,
           config->dos.blacklist_duration_sec);
}
