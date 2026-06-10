/* CLI 入口：负责配置检查、自测、probe 注入和运行主循环。 */

#include "xgw_config.h"
#include "xgw_dataplane.h"
#include "xgw_frame.h"
#include "xgw_obfs.h"
#include "xgw_outbound.h"
#include "xgw_policy.h"
#include "xgw_runtime.h"
#include "xgw_session.h"
#include "xgw_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(void) {
    printf("xgw validate <config>\n");
    printf("xgw explain <config>\n");
    printf("xgw probe-policy <config> <ip> [domain]\n");
    printf("xgw selftest <config>\n");
    printf("xgw probe-send <config> <host:port>\n");
    printf("xgw outbound-smoke <config> <tcp|udp> <host:port>\n");
    printf("xgw run <config>\n");
}

/* 读取并打印配置，便于人工确认展开后的运行参数。 */
static int run_validate(const char *config_path, int explain) {
    xgw_runtime_config_t config;
    char error[256];

    if (!xgw_config_load(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "config error: %s\n", error);
        return 1;
    }

    if (explain) {
        xgw_config_print(&config);
        printf("design_focus=fixed-path, live traffic, anti-jitter, simple-config\n");
        printf("language_choice=c-core because deterministic pacing/FEC/session hot path fits C better than Go\n");
        printf("stable_segment=guangzhou->hongkong private line\n");
        printf("unstable_segments=mobile->guangzhou,hongkong->overseas,overseas->tiktok\n");
    } else {
        printf("config ok: %s (%s, %s)\n",
               config.node_name,
               xgw_role_name(config.role),
               xgw_profile_name(config.profile.mode));
    }
    return 0;
}

/* 快速验证当前白名单 / 黑名单 / DOS 规则是否符合预期。 */
static int run_probe_policy(const char *config_path, const char *ip, const char *domain) {
    xgw_runtime_config_t config;
    xgw_dos_protector_t protector;
    const char *reason = NULL;
    char error[256];
    time_t now = time(NULL);

    if (!xgw_config_load(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "config error: %s\n", error);
        return 1;
    }
    xgw_dos_init(&protector, &config.dos);

    printf("policy_ip_allow=%s\n",
           xgw_allow_policy_allow_ip(&config.allow_policy, ip, now) ? "true" : "false");
    if (domain != NULL) {
        printf("policy_domain_match=%s\n",
               xgw_allow_policy_match_domain(&config.allow_policy, domain) ? "true" : "false");
    }
    printf("dos_allow=%s\n", xgw_dos_allow(&protector, ip, now, &reason) ? "true" : "false");
    printf("dos_reason=%s\n", reason == NULL ? "ok" : reason);
    return 0;
}

/* 本地自测：验证分片、FEC 恢复与 replay 去重。 */
static int run_selftest(const char *config_path) {
    xgw_runtime_config_t config;
    xgw_dataplane_t *dp = NULL;
    xgw_session_t *session;
    xgw_frame_batch_t *batch = NULL;
    xgw_reassembly_result_t *result = NULL;
    xgw_packet_t *packet = NULL;
    uint8_t *fec_frame = NULL;
    size_t fec_len = 0;
    char error[256];
    char summary[256];
    uint8_t message[1600];
    size_t i;

    for (i = 0; i < sizeof(message); ++i) {
        message[i] = (uint8_t) ('A' + (i % 26U));
    }
    if (!xgw_config_load(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "config error: %s\n", error);
        return 1;
    }
    dp = (xgw_dataplane_t *) calloc(1, sizeof(*dp));
    batch = (xgw_frame_batch_t *) calloc(1, sizeof(*batch));
    result = (xgw_reassembly_result_t *) calloc(1, sizeof(*result));
    packet = (xgw_packet_t *) calloc(1, sizeof(*packet));
    fec_frame = (uint8_t *) calloc(1, XGW_MAX_FRAME_SIZE);
    if (dp == NULL || batch == NULL || result == NULL || packet == NULL || fec_frame == NULL) {
        fprintf(stderr, "selftest error: allocation failed\n");
        free(dp);
        free(batch);
        free(result);
        free(packet);
        free(fec_frame);
        return 1;
    }
    xgw_dataplane_init(dp, &config);
    session = xgw_session_upsert(&dp->sessions, 1001U, config.allow_policy.group_id, "10.0.0.10", "106.75.31.69", 51830U, time(NULL));
    if (session == NULL) {
        fprintf(stderr, "selftest error: unable to create session\n");
        free(dp);
        free(batch);
        free(result);
        free(packet);
        free(fec_frame);
        return 1;
    }
    if (!xgw_dataplane_build_outbound(dp,
                                      session,
                                      message,
                                      sizeof(message),
                                      batch,
                                      fec_frame,
                                      XGW_MAX_FRAME_SIZE,
                                      &fec_len)) {
        fprintf(stderr, "selftest error: failed to build outbound frames (%s)\n", xgw_dataplane_last_error());
        free(dp);
        free(batch);
        free(result);
        free(packet);
        free(fec_frame);
        return 1;
    }
    printf("selftest.frames=%zu\n", batch->frame_count);
    printf("selftest.fec=%zu\n", fec_len);

    for (i = 0; i < batch->frame_count; ++i) {
        memset(packet, 0, sizeof(*packet));
        memcpy(packet->data, batch->frames[i], batch->frame_lengths[i]);
        packet->data_len = batch->frame_lengths[i];
        snprintf(packet->remote_host, sizeof(packet->remote_host), "%s", "106.75.31.69");
        packet->remote_port = 51830U;
        if (i == 1U && batch->frame_count > 1U) {
            continue;
        }
        xgw_dataplane_process_frame(dp, packet, result, summary, sizeof(summary));
        printf("selftest.step[%zu]=%s\n", i, summary);
    }

    if (fec_len > 0U) {
        memset(packet, 0, sizeof(*packet));
        memcpy(packet->data, fec_frame, fec_len);
        packet->data_len = fec_len;
        snprintf(packet->remote_host, sizeof(packet->remote_host), "%s", "106.75.31.69");
        packet->remote_port = 51830U;
        xgw_dataplane_process_frame(dp, packet, result, summary, sizeof(summary));
        printf("selftest.fec_step=%s\n", summary);
        if (result->packet_len > 0U) {
            printf("selftest.reassembled_len=%zu\n", result->packet_len);
            printf("selftest.reassembled_head=%.*s\n", 32, result->packet);
        }
    }

    printf("selftest.replay_accept_1=%d\n", xgw_replay_window_accept(&session->replay_window, 9999U, config.profile.reorder_window));
    printf("selftest.replay_accept_2=%d\n", xgw_replay_window_accept(&session->replay_window, 9999U, config.profile.reorder_window));
    free(dp);
    free(batch);
    free(result);
    free(packet);
    free(fec_frame);
    return 0;
}

/* 解析 host:port 参数。 */
static int split_host_port_arg(const char *value, char *host, size_t host_len, unsigned short *port) {
    const char *colon = strrchr(value, ':');
    char *end = NULL;
    unsigned long parsed;
    if (colon == NULL || colon == value) {
        return 0;
    }
    snprintf(host, host_len, "%.*s", (int) (colon - value), value);
    parsed = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || parsed > 65535UL) {
        return 0;
    }
    *port = (unsigned short) parsed;
    return 1;
}

/* 发送一个最小合法探针帧到目标地址，用于远程 smoke。 */
static int run_probe_send(const char *config_path, const char *remote) {
    xgw_runtime_config_t config;
    xgw_dataplane_t *dp = NULL;
    xgw_session_t *session = NULL;
    xgw_frame_batch_t *batch = NULL;
    xgw_udp_socket_t udp;
    xgw_obfs_t obfs;
    uint8_t *fec_frame = NULL;
    size_t fec_len = 0;
    char error[256];
    char host[64];
    unsigned short port = 0;
    uint8_t payload[256];
    size_t i;

    if (!split_host_port_arg(remote, host, sizeof(host), &port)) {
        fprintf(stderr, "probe-send error: invalid host:port %s\n", remote);
        return 1;
    }
    for (i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t) ('a' + (i % 26U));
    }
    if (!xgw_config_load(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "config error: %s\n", error);
        return 1;
    }
    xgw_obfs_init(&obfs, config.obfs_mode, config.obfs_key);
    dp = (xgw_dataplane_t *) calloc(1, sizeof(*dp));
    batch = (xgw_frame_batch_t *) calloc(1, sizeof(*batch));
    fec_frame = (uint8_t *) calloc(1, XGW_MAX_FRAME_SIZE);
    if (dp == NULL || batch == NULL || fec_frame == NULL) {
        fprintf(stderr, "probe-send error: allocation failed\n");
        free(dp);
        free(batch);
        free(fec_frame);
        return 1;
    }
    memset(&udp, 0, sizeof(udp));
    xgw_dataplane_init(dp, &config);
    session = xgw_session_upsert(&dp->sessions, 777U, config.allow_policy.group_id, "10.23.9.9", host, port, time(NULL));
    if (session == NULL) {
        fprintf(stderr, "probe-send error: cannot create session\n");
        free(dp);
        free(batch);
        free(fec_frame);
        return 1;
    }
    if (!xgw_udp_listen(&udp, "0.0.0.0", 0U)) {
        fprintf(stderr, "probe-send error: cannot open local udp socket\n");
        free(dp);
        free(batch);
        free(fec_frame);
        return 1;
    }
    if (!xgw_dataplane_build_outbound(dp, session, payload, sizeof(payload), batch, fec_frame, XGW_MAX_FRAME_SIZE, &fec_len)) {
        fprintf(stderr, "probe-send error: %s\n", xgw_dataplane_last_error());
        xgw_udp_close(&udp);
        free(dp);
        free(batch);
        free(fec_frame);
        return 1;
    }
    for (i = 0; i < batch->frame_count; ++i) {
        uint8_t wire[65535];
        const uint8_t *payload_ptr = batch->frames[i];
        size_t payload_len = batch->frame_lengths[i];
        if (xgw_obfs_enabled(&obfs)) {
            payload_len = xgw_obfs_encode(&obfs, batch->frames[i], batch->frame_lengths[i], wire, sizeof(wire));
            payload_ptr = wire;
        }
        if (!xgw_udp_send(&udp, payload_ptr, payload_len, host, port)) {
            fprintf(stderr, "probe-send error: send data frame failed\n");
            xgw_udp_close(&udp);
            free(dp);
            free(batch);
            free(fec_frame);
            return 1;
        }
    }
    if (fec_len > 0U) {
        uint8_t wire[65535];
        const uint8_t *payload_ptr = fec_frame;
        size_t payload_len = fec_len;
        if (xgw_obfs_enabled(&obfs)) {
            payload_len = xgw_obfs_encode(&obfs, fec_frame, fec_len, wire, sizeof(wire));
            payload_ptr = wire;
        }
        if (!xgw_udp_send(&udp, payload_ptr, payload_len, host, port)) {
            fprintf(stderr, "probe-send error: send fec frame failed\n");
            xgw_udp_close(&udp);
            free(dp);
            free(batch);
            free(fec_frame);
            return 1;
        }
    }
    printf("probe-send ok frames=%zu fec=%zu remote=%s:%u\n", batch->frame_count, fec_len, host, port);
    xgw_udp_close(&udp);
    free(dp);
    free(batch);
    free(fec_frame);
    return 0;
}

static int run_outbound_smoke(const char *config_path, const char *proto, const char *target) {
    xgw_runtime_config_t config;
    xgw_outbound_t outbound;
    xgw_outbound_type_t outbound_type = XGW_OUTBOUND_DIRECT;
    xgw_packet_t packet;
    char error[256];
    char host[64];
    unsigned short port = 0;
    uint8_t buf[256];
    const xgw_pool_node_t *node = NULL;
    xgw_pool_t pool;
    int n;

    if (!split_host_port_arg(target, host, sizeof(host), &port)) {
        fprintf(stderr, "outbound-smoke error: invalid host:port %s\n", target);
        return 1;
    }
    if (!xgw_config_load(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "config error: %s\n", error);
        return 1;
    }
    memset(&outbound, 0, sizeof(outbound));
    xgw_pool_init(&pool);
    pool = config.pool;
    if (!xgw_outbound_parse_type(config.outbound_type, &outbound_type)) {
        fprintf(stderr, "outbound-smoke error: invalid outbound_type %s\n", config.outbound_type);
        return 1;
    }
    if (config.outbound_host[0] == '\0' || config.outbound_port == 0) {
        node = xgw_pool_select_best(&pool);
        if (node == NULL) {
            fprintf(stderr, "outbound-smoke error: no outbound target configured\n");
            return 1;
        }
        if (!xgw_outbound_parse_type(node->outbound_type, &outbound_type)) {
            fprintf(stderr, "outbound-smoke error: invalid pool outbound type %s\n", node->outbound_type);
            return 1;
        }
        if (!xgw_outbound_open(&outbound,
                               outbound_type,
                               node->name,
                               node->host,
                               node->port,
                               node->username,
                               node->password,
                               error,
                               sizeof(error))) {
            fprintf(stderr, "outbound-smoke error: %s\n", error);
            return 1;
        }
    } else {
        if (!xgw_outbound_open(&outbound,
                               outbound_type,
                               "default",
                               config.outbound_host,
                               config.outbound_port,
                               config.outbound_username,
                               config.outbound_password,
                               error,
                               sizeof(error))) {
            fprintf(stderr, "outbound-smoke error: %s\n", error);
            return 1;
        }
    }
    if (strcmp(proto, "tcp") == 0) {
        const char *msg = "xgw-outbound-tcp";
        printf("outbound-smoke.stage open\n");
        fflush(stdout);
        if (!xgw_outbound_connect_tcp(&outbound, host, port, error, sizeof(error))) {
            fprintf(stderr, "outbound-smoke error: %s\n", error);
            xgw_outbound_close(&outbound);
            return 1;
        }
        printf("outbound-smoke.stage connected\n");
        fflush(stdout);
        if (!xgw_outbound_send_tcp(&outbound, (const uint8_t *) msg, strlen(msg))) {
            fprintf(stderr, "outbound-smoke error: tcp send failed\n");
            xgw_outbound_close(&outbound);
            return 1;
        }
        printf("outbound-smoke.stage sent\n");
        fflush(stdout);
        n = xgw_outbound_recv_tcp(&outbound, buf, sizeof(buf), 5000);
        if (n <= 0) {
            fprintf(stderr, "outbound-smoke error: tcp recv failed\n");
            xgw_outbound_close(&outbound);
            return 1;
        }
        printf("outbound-smoke.stage received\n");
        fflush(stdout);
        printf("outbound-smoke ok proto=tcp outbound=%s target=%s:%u echo=%.*s\n",
               xgw_outbound_type_name(outbound_type),
               host,
               port,
               n,
               buf);
        xgw_outbound_close(&outbound);
        return 0;
    }
    if (strcmp(proto, "udp") == 0) {
        const char *msg = "xgw-outbound-udp";
        memset(&packet, 0, sizeof(packet));
        printf("outbound-smoke.stage send\n");
        fflush(stdout);
        if (!xgw_outbound_send(&outbound, (const uint8_t *) msg, strlen(msg), host, port)) {
            fprintf(stderr, "outbound-smoke error: udp send failed\n");
            xgw_outbound_close(&outbound);
            return 1;
        }
        printf("outbound-smoke.stage wait-recv\n");
        fflush(stdout);
        if (!xgw_outbound_recv(&outbound, &packet, 5000)) {
            fprintf(stderr, "outbound-smoke error: udp recv failed\n");
            xgw_outbound_close(&outbound);
            return 1;
        }
        printf("outbound-smoke.stage received\n");
        fflush(stdout);
        printf("outbound-smoke ok proto=udp outbound=%s target=%s:%u echo=%.*s\n",
               xgw_outbound_type_name(outbound_type),
               host,
               port,
               (int) packet.data_len,
               packet.data);
        xgw_outbound_close(&outbound);
        return 0;
    }
    fprintf(stderr, "outbound-smoke error: unsupported proto %s\n", proto);
    xgw_outbound_close(&outbound);
    return 1;
}

/* 主程序入口，根据子命令分发到具体逻辑。 */
int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "validate") == 0) {
        return run_validate(argv[2], 0);
    }
    if (strcmp(argv[1], "explain") == 0) {
        return run_validate(argv[2], 1);
    }
    if (strcmp(argv[1], "probe-policy") == 0) {
        return run_probe_policy(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    }
    if (strcmp(argv[1], "selftest") == 0) {
        return run_selftest(argv[2]);
    }
    if (strcmp(argv[1], "probe-send") == 0) {
        if (argc < 4) {
            print_usage();
            return 1;
        }
        return run_probe_send(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "outbound-smoke") == 0) {
        if (argc < 5) {
            print_usage();
            return 1;
        }
        return run_outbound_smoke(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "run") == 0) {
        char error[256];
        xgw_runtime_config_t config;
        if (!xgw_config_load(argv[2], &config, error, sizeof(error))) {
            fprintf(stderr, "config error: %s\n", error);
            return 1;
        }
        if (!xgw_runtime_run(&config, error, sizeof(error))) {
            fprintf(stderr, "runtime error: %s\n", error);
            return 1;
        }
        return 0;
    }

    print_usage();
    return 1;
}
