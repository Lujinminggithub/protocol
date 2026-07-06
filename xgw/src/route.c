/* 多线路管理器：把 Go 控制面的选路结果落实成 C 热路径可消费的 active line。 */

#include "xgw_route.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

static int endpoint_address_valid(const xgw_endpoint_t *endpoint) {
    return endpoint != NULL && endpoint->address[0] != '\0';
}

int xgw_route_endpoint_address(const xgw_node_hops_t *hops,
                               const xgw_endpoint_t *endpoint,
                               char *out,
                               size_t out_len) {
    xgw_role_t local_role = XGW_ROLE_UNKNOWN;
    if (hops != NULL) {
        local_role = hops->local_role;
    }
    if (endpoint == NULL || out == NULL || out_len == 0U) {
        return 0;
    }
    return xgw_endpoint_pick_address(endpoint, local_role, endpoint->role, out, out_len);
}

static int route_same_id(const char *a, const char *b) {
    return strcmp(a == NULL ? "" : a, b == NULL ? "" : b) == 0;
}

const char *xgw_line_state_name(xgw_line_state_t state) {
    switch (state) {
    case XGW_LINE_ACTIVE:
        return "active";
    case XGW_LINE_CANDIDATE:
        return "candidate";
    case XGW_LINE_DRAINING:
        return "draining";
    case XGW_LINE_IDLE:
    default:
        return "idle";
    }
}

int xgw_route_resolve_hops(const xgw_fixed_path_t *path,
                           xgw_role_t role,
                           const char *hop_name,
                           xgw_node_hops_t *hops) {
    size_t i;
    if (path == NULL || hops == NULL) {
        return 0;
    }
    memset(hops, 0, sizeof(*hops));
    hops->local_role = role;
    for (i = 0; i < path->hop_count; ++i) {
        const xgw_endpoint_t *hop = &path->hops[i];
        if ((hop_name != NULL && hop_name[0] != '\0' && strcmp(hop->name, hop_name) == 0) ||
            ((hop_name == NULL || hop_name[0] == '\0') && hop->role == role && hops->current == NULL)) {
            hops->current = hop;
            if (i > 0U) {
                hops->previous = &path->hops[i - 1U];
            }
            if (i + 1U < path->hop_count) {
                hops->next = &path->hops[i + 1U];
            }
            return endpoint_address_valid(hops->current);
        }
    }
    return 0;
}

static int add_runtime_line(xgw_route_manager_t *manager,
                            const xgw_line_config_t *line,
                            xgw_role_t role,
                            const char *hop_name) {
    xgw_line_runtime_t *runtime;
    if (manager == NULL || line == NULL || !line->enabled || manager->line_count >= XGW_MAX_LINES) {
        return 0;
    }
    runtime = &manager->lines[manager->line_count];
    memset(runtime, 0, sizeof(*runtime));
    runtime->config = line;
    runtime->state = XGW_LINE_IDLE;
    if (!xgw_route_resolve_hops(&line->path, role, hop_name, &runtime->hops)) {
        return 0;
    }
    manager->line_count++;
    return 1;
}

int xgw_route_manager_init(xgw_route_manager_t *manager,
                           const xgw_line_table_config_t *lines,
                           const xgw_fixed_path_t *fallback_path,
                           xgw_role_t role,
                           const char *hop_name) {
    size_t i;
    time_t now = time(NULL);
    static xgw_line_config_t fallback_line;
    if (manager == NULL) {
        return 0;
    }
    memset(manager, 0, sizeof(*manager));
    manager->active_index = -1;
    manager->candidate_index = -1;
    manager->draining_index = -1;
    manager->drain_timeout_sec = lines == NULL || lines->drain_timeout_sec == 0U ? 5U : lines->drain_timeout_sec;
    if (lines != NULL) {
        for (i = 0; i < lines->line_count; ++i) {
            add_runtime_line(manager, &lines->lines[i], role, hop_name);
        }
    }
    if (manager->line_count == 0U && fallback_path != NULL && fallback_path->hop_count > 0U) {
        memset(&fallback_line, 0, sizeof(fallback_line));
        snprintf(fallback_line.id, sizeof(fallback_line.id), "%s", "default");
        fallback_line.path = *fallback_path;
        fallback_line.enabled = 1;
        fallback_line.priority = 0;
        add_runtime_line(manager, &fallback_line, role, hop_name);
    }
    if (manager->line_count == 0U) {
        return 0;
    }
    manager->active_index = 0;
    if (lines != NULL && lines->default_line[0] != '\0') {
        for (i = 0; i < manager->line_count; ++i) {
            if (route_same_id(manager->lines[i].config->id, lines->default_line)) {
                manager->active_index = (int) i;
                break;
            }
        }
    }
    manager->lines[manager->active_index].state = XGW_LINE_ACTIVE;
    manager->lines[manager->active_index].state_since = now;
    return 1;
}

const xgw_line_runtime_t *xgw_route_active(const xgw_route_manager_t *manager) {
    if (manager == NULL || manager->active_index < 0 || (size_t) manager->active_index >= manager->line_count) {
        return NULL;
    }
    return &manager->lines[manager->active_index];
}

const xgw_line_runtime_t *xgw_route_candidate(const xgw_route_manager_t *manager) {
    if (manager == NULL || manager->candidate_index < 0 || (size_t) manager->candidate_index >= manager->line_count) {
        return NULL;
    }
    return &manager->lines[manager->candidate_index];
}

const xgw_line_runtime_t *xgw_route_draining(const xgw_route_manager_t *manager) {
    if (manager == NULL || manager->draining_index < 0 || (size_t) manager->draining_index >= manager->line_count) {
        return NULL;
    }
    return &manager->lines[manager->draining_index];
}

xgw_line_runtime_t *xgw_route_find_mut(xgw_route_manager_t *manager, const char *line_id) {
    size_t i;
    if (manager == NULL || line_id == NULL || line_id[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < manager->line_count; ++i) {
        if (route_same_id(manager->lines[i].config->id, line_id)) {
            return &manager->lines[i];
        }
    }
    return NULL;
}

const xgw_line_runtime_t *xgw_route_find(const xgw_route_manager_t *manager, const char *line_id) {
    return xgw_route_find_mut((xgw_route_manager_t *) manager, line_id);
}

static int endpoint_matches_peer(const xgw_endpoint_t *endpoint, const char *host, uint16_t port) {
    const char *candidates[3];
    size_t i;
    if (endpoint == NULL || host == NULL || host[0] == '\0' || port == 0U) {
        return 0;
    }
    candidates[0] = endpoint->address;
    candidates[1] = endpoint->public_address;
    candidates[2] = endpoint->private_address;
    for (i = 0U; i < 3U; ++i) {
        const char *addr = candidates[i];
        const char *colon;
        unsigned long parsed_port;
        char endpoint_host[XGW_MAX_NAME_LEN];
        char *end = NULL;
        if (addr == NULL || addr[0] == '\0') {
            continue;
        }
        colon = strrchr(addr, ':');
        if (colon == NULL || colon == addr) {
            continue;
        }
        snprintf(endpoint_host, sizeof(endpoint_host), "%.*s", (int) (colon - addr), addr);
        parsed_port = strtoul(colon + 1, &end, 10);
        if (end == colon + 1 || *end != '\0' || parsed_port > 65535UL) {
            continue;
        }
        if (strcmp(endpoint_host, host) == 0 && (uint16_t) parsed_port == port) {
            return 1;
        }
    }
    return 0;
}

const xgw_line_runtime_t *xgw_route_find_by_peer(const xgw_route_manager_t *manager,
                                                 const char *host,
                                                 uint16_t port) {
    size_t i;
    const xgw_line_runtime_t *active;
    if (manager == NULL || host == NULL || host[0] == '\0' || port == 0U) {
        return NULL;
    }
    active = xgw_route_active(manager);
    if (active != NULL &&
        (endpoint_matches_peer(active->hops.previous, host, port) ||
         endpoint_matches_peer(active->hops.next, host, port))) {
        return active;
    }
    for (i = 0; i < manager->line_count; ++i) {
        const xgw_line_runtime_t *line = &manager->lines[i];
        if (line == active || line->state == XGW_LINE_IDLE) {
            continue;
        }
        if (endpoint_matches_peer(line->hops.previous, host, port) ||
            endpoint_matches_peer(line->hops.next, host, port)) {
            return line;
        }
    }
    return NULL;
}

static int runtime_index(const xgw_route_manager_t *manager, const xgw_line_runtime_t *line) {
    size_t i;
    if (manager == NULL || line == NULL) {
        return -1;
    }
    for (i = 0; i < manager->line_count; ++i) {
        if (&manager->lines[i] == line) {
            return (int) i;
        }
    }
    return -1;
}

int xgw_route_prewarm(xgw_route_manager_t *manager, const char *line_id, time_t now) {
    xgw_line_runtime_t *line = xgw_route_find_mut(manager, line_id);
    int index = runtime_index(manager, line);
    if (line == NULL || index < 0) {
        printf("route.prewarm.fail reason=line_not_found line=%s\n", line_id == NULL ? "" : line_id);
        fflush(stdout);
        return 0;
    }
    if (index == manager->active_index) {
        return 1;
    }
    if (manager->draining_index == index) {
        printf("route.prewarm.fail reason=draining line=%s\n", line_id);
        fflush(stdout);
        return 0;
    }
    if (manager->candidate_index >= 0 && manager->candidate_index != index) {
        manager->lines[manager->candidate_index].state = XGW_LINE_IDLE;
    }
    manager->candidate_index = index;
    line->state = XGW_LINE_CANDIDATE;
    line->state_since = now;
    {
        char next_addr[XGW_MAX_NAME_LEN] = "";
        char prev_addr[XGW_MAX_NAME_LEN] = "";
        xgw_route_endpoint_address(&line->hops, line->hops.next, next_addr, sizeof(next_addr));
        xgw_route_endpoint_address(&line->hops, line->hops.previous, prev_addr, sizeof(prev_addr));
        printf("route.prewarm line=%s next=%s previous=%s\n",
               line->config->id,
               next_addr,
               prev_addr);
    }
    fflush(stdout);
    return 1;
}

int xgw_route_set_active(xgw_route_manager_t *manager, const char *line_id, time_t now) {
    xgw_line_runtime_t *line = xgw_route_find_mut(manager, line_id);
    int index = runtime_index(manager, line);
    if (line == NULL || index < 0) {
        return 0;
    }
    if (index == manager->active_index) {
        return 1;
    }
    if (manager->active_index >= 0 && (size_t) manager->active_index < manager->line_count) {
        xgw_line_runtime_t *old = &manager->lines[manager->active_index];
        old->state = XGW_LINE_DRAINING;
        old->state_since = now;
        old->drain_until = now + (time_t) manager->drain_timeout_sec;
        manager->draining_index = manager->active_index;
        printf("route.drain old_line=%s until=%ld\n", old->config->id, (long) old->drain_until);
        fflush(stdout);
    }
    manager->active_index = index;
    manager->candidate_index = -1;
    line->state = XGW_LINE_ACTIVE;
    line->state_since = now;
    line->drain_until = 0;
    {
        char next_addr[XGW_MAX_NAME_LEN] = "";
        char prev_addr[XGW_MAX_NAME_LEN] = "";
        xgw_route_endpoint_address(&line->hops, line->hops.next, next_addr, sizeof(next_addr));
        xgw_route_endpoint_address(&line->hops, line->hops.previous, prev_addr, sizeof(prev_addr));
        printf("route.active line=%s next=%s previous=%s\n",
               line->config->id,
               next_addr,
               prev_addr);
    }
    fflush(stdout);
    return 1;
}

int xgw_route_request_active(xgw_route_manager_t *manager, const char *line_id, time_t now) {
    if (manager == NULL || line_id == NULL || line_id[0] == '\0') {
        printf("route.active.request.fail reason=invalid line=%s\n", line_id == NULL ? "" : line_id);
        fflush(stdout);
        return 0;
    }
    if (manager->active_index >= 0 &&
        (size_t) manager->active_index < manager->line_count &&
        route_same_id(manager->lines[manager->active_index].config->id, line_id)) {
        manager->requested_active_line[0] = '\0';
        return 1;
    }
    if (xgw_route_find(manager, line_id) == NULL) {
        printf("route.active.request.fail reason=line_not_found line=%s\n", line_id);
        fflush(stdout);
        return 0;
    }
    if (manager->candidate_index < 0 ||
        !route_same_id(manager->lines[manager->candidate_index].config->id, line_id)) {
        if (!xgw_route_prewarm(manager, line_id, now)) {
            return 0;
        }
    }
    snprintf(manager->requested_active_line, sizeof(manager->requested_active_line), "%s", line_id);
    printf("route.active.request line=%s\n", line_id);
    fflush(stdout);
    return 1;
}

int xgw_route_commit_requested(xgw_route_manager_t *manager, time_t now) {
    char line_id[XGW_MAX_NAME_LEN];
    if (manager == NULL || manager->requested_active_line[0] == '\0') {
        return 1;
    }
    snprintf(line_id, sizeof(line_id), "%s", manager->requested_active_line);
    if (!xgw_route_set_active(manager, line_id, now)) {
        return 0;
    }
    manager->requested_active_line[0] = '\0';
    return 1;
}

int xgw_route_drain(xgw_route_manager_t *manager, const char *line_id, time_t now) {
    xgw_line_runtime_t *line = xgw_route_find_mut(manager, line_id);
    int index = runtime_index(manager, line);
    if (line == NULL || index < 0 || index == manager->active_index) {
        return 0;
    }
    if (manager->candidate_index == index) {
        manager->candidate_index = -1;
    }
    line->state = XGW_LINE_DRAINING;
    line->state_since = now;
    line->drain_until = now + (time_t) manager->drain_timeout_sec;
    manager->draining_index = index;
    printf("route.drain line=%s until=%ld\n", line->config->id, (long) line->drain_until);
    fflush(stdout);
    return 1;
}

void xgw_route_tick(xgw_route_manager_t *manager, time_t now) {
    if (manager == NULL || manager->draining_index < 0 || (size_t) manager->draining_index >= manager->line_count) {
        return;
    }
    if (manager->lines[manager->draining_index].drain_until != 0 &&
        now >= manager->lines[manager->draining_index].drain_until) {
        printf("route.drain.done line=%s\n", manager->lines[manager->draining_index].config->id);
        fflush(stdout);
        manager->lines[manager->draining_index].state = XGW_LINE_IDLE;
        manager->lines[manager->draining_index].drain_until = 0;
        manager->draining_index = -1;
    }
}

int xgw_route_apply_command(xgw_route_manager_t *manager, const char *command, time_t now) {
    char op[64];
    char line_id[XGW_MAX_NAME_LEN];
    if (manager == NULL || command == NULL || command[0] == '\0') {
        return 0;
    }
    if (sscanf(command, "%63s %63s", op, line_id) != 2) {
        printf("route.command.fail reason=parse command=%s\n", command);
        fflush(stdout);
        return 0;
    }
    if (strcmp(command, manager->last_command) == 0) {
        return 1;
    }
    snprintf(manager->last_command, sizeof(manager->last_command), "%s", command);
    printf("route.command op=%s line=%s\n", op, line_id);
    fflush(stdout);
    if (strcmp(op, "PREWARM_LINE") == 0 || strcmp(op, "prewarm") == 0) {
        return xgw_route_prewarm(manager, line_id, now);
    }
    if (strcmp(op, "SET_ACTIVE_LINE") == 0 || strcmp(op, "active") == 0) {
        return xgw_route_request_active(manager, line_id, now);
    }
    if (strcmp(op, "DRAIN_LINE") == 0 || strcmp(op, "drain") == 0) {
        return xgw_route_drain(manager, line_id, now);
    }
    printf("route.command.fail reason=unknown_op op=%s line=%s\n", op, line_id);
    fflush(stdout);
    return 0;
}

int xgw_route_poll_control_file(xgw_route_manager_t *manager, const char *path, time_t now) {
    FILE *fp;
    char command[256];
    if (manager == NULL || path == NULL || path[0] == '\0') {
        return 1;
    }
#ifdef _WIN32
    if (fopen_s(&fp, path, "r") != 0) {
        fp = NULL;
    }
#else
    fp = fopen(path, "r");
#endif
    if (fp == NULL) {
        return 1;
    }
    if (fgets(command, sizeof(command), fp) == NULL) {
        fclose(fp);
        return 1;
    }
    fclose(fp);
    command[strcspn(command, "\r\n")] = '\0';
    return xgw_route_apply_command(manager, command, now);
}

int xgw_route_write_metrics(const xgw_route_manager_t *manager,
                            const char *path,
                            size_t session_count,
                            uint32_t available_mbps) {
    FILE *fp;
    size_t i;
    if (manager == NULL || path == NULL || path[0] == '\0') {
        return 1;
    }
#ifdef _WIN32
    if (fopen_s(&fp, path, "w") != 0) {
        fp = NULL;
    }
#else
    fp = fopen(path, "w");
#endif
    if (fp == NULL) {
        return 0;
    }
    fprintf(fp, "{\n");
    for (i = 0; i < manager->line_count; ++i) {
        const xgw_line_runtime_t *line = &manager->lines[i];
        int current_sessions = line->state == XGW_LINE_ACTIVE ? (int) session_count : 0;
        int health_penalty = 0;
        const char *comma = (i + 1U < manager->line_count) ? "," : "";
        if (line->state == XGW_LINE_DRAINING) {
            health_penalty = 60;
        } else if (line->state == XGW_LINE_IDLE) {
            health_penalty = 10;
        }
        fprintf(fp,
                "  \"%s\": {\"latency_millis\": 0, \"jitter_millis\": 0, \"loss_ppm\": 0, \"available_mbps\": %u, \"current_sessions\": %d, \"health_penalty\": %d, \"bandwidth_penalty\": 0, \"probe_ok\": true, \"probe_error\": \"\", \"state\": \"%s\", \"priority\": %d, \"active\": %s, \"candidate\": %s, \"draining\": %s, \"drain_until\": %ld}%s\n",
                line->config == NULL ? "" : line->config->id,
                available_mbps,
                current_sessions,
                health_penalty,
                xgw_line_state_name(line->state),
                line->config == NULL ? 0 : line->config->priority,
                line->state == XGW_LINE_ACTIVE ? "true" : "false",
                line->state == XGW_LINE_CANDIDATE ? "true" : "false",
                line->state == XGW_LINE_DRAINING ? "true" : "false",
                (long) line->drain_until,
                comma);
    }
    fprintf(fp, "}\n");
    fclose(fp);
    return 1;
}
