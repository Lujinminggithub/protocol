/* 数据面核心处理：入站解析、策略检查、FEC 重组、出站构建。 */

#include "xgw_dataplane.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static char g_xgw_dataplane_error[128];

static void set_last_error(const char *text) {
    snprintf(g_xgw_dataplane_error, sizeof(g_xgw_dataplane_error), "%s", text == NULL ? "" : text);
}

/* 返回最近一次出站构建失败原因。 */
const char *xgw_dataplane_last_error(void) {
    return g_xgw_dataplane_error;
}

/* 初始化数据面对象。 */
void xgw_dataplane_init(xgw_dataplane_t *dp, const xgw_runtime_config_t *config) {
    memset(dp, 0, sizeof(*dp));
    dp->config = *config;
    dp->negotiation.authenticated = config->auth_token[0] != '\0';
    dp->negotiation.udp_enabled = config->enable_udp;
    dp->negotiation.local_rx_bps = config->advertised_rx_bps;
    dp->negotiation.peer_rx_bps = config->advertised_tx_bps;
    dp->negotiation.peer_rx_auto = config->advertised_tx_bps == 0;
    dp->negotiation.congestion_mode = config->congestion_mode;
    dp->negotiation.bbr_profile = config->bbr_profile;
    snprintf(dp->negotiation.auth_token, sizeof(dp->negotiation.auth_token), "%s", config->auth_token);
    xgw_acl_init(&dp->acl);
    xgw_acl_add_outbound(&dp->acl,
                         "default",
                         config->outbound_type[0] != '\0' ? config->outbound_type :
                         (config->outbound_host[0] != '\0' ? "fixed" : "direct"),
                         config->outbound_host,
                         config->outbound_port);
    xgw_pool_init(&dp->pool);
    if (config->outbound_host[0] != '\0' && config->outbound_port > 0U) {
        xgw_pool_add_node(&dp->pool,
                          "default",
                          config->outbound_type[0] != '\0' ? config->outbound_type : "fixed",
                          config->outbound_host,
                          config->outbound_port,
                          config->outbound_username,
                          config->outbound_password,
                          10,
                          0,
                          100);
    }
    xgw_session_table_init(&dp->sessions);
    xgw_fec_codec_init(&dp->fec, config->profile.fec_data_shards, config->profile.fec_parity_shards);
    xgw_dos_init(&dp->dos, &config->dos);
}

/* 处理一帧入站数据，并生成可读摘要。 */
int xgw_dataplane_process_frame(xgw_dataplane_t *dp, const xgw_packet_t *packet, xgw_reassembly_result_t *result, char *summary, size_t summary_len) {
    xgw_header_t header;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    xgw_session_t *session;
    time_t now = time(NULL);
    const char *reason = NULL;

    if (!dp->negotiation.udp_enabled) {
        snprintf(summary,
                 summary_len,
                 "drop=udp_disabled remote=%s packet_len=%zu",
                 packet->remote_host,
                 packet->data_len);
        return 0;
    }

    if (!xgw_dos_allow(&dp->dos, packet->remote_host, now, &reason)) {
        snprintf(summary,
                 summary_len,
                 "drop=dos remote=%s reason=%s packet_len=%zu",
                 packet->remote_host,
                 reason == NULL ? "blocked" : reason,
                 packet->data_len);
        return 0;
    }
    if (!xgw_header_decode(packet->data, packet->data_len, &header, &payload, &payload_len)) {
        snprintf(summary,
                 summary_len,
                 "drop=header_decode_fail remote=%s packet_len=%zu",
                 packet->remote_host,
                 packet->data_len);
        return 0;
    }
    session = xgw_session_upsert(&dp->sessions,
                                 header.session_id,
                                 dp->config.allow_policy.group_id,
                                 "",
                                 packet->remote_host,
                                 packet->remote_port,
                                 now);
    if (session == NULL) {
        snprintf(summary,
                 summary_len,
                 "drop=session_table_full type=%s session=%u seq=%llu",
                 xgw_message_type_name(header.type),
                 header.session_id,
                 (unsigned long long) header.sequence);
        return 0;
    }
    if (!xgw_replay_window_accept(&session->replay_window, header.sequence, dp->config.profile.reorder_window)) {
        snprintf(summary,
                 summary_len,
                 "drop=replay type=%s session=%u seq=%llu",
                 xgw_message_type_name(header.type),
                 session->id,
                 (unsigned long long) header.sequence);
        return 0;
    }
    xgw_session_touch(session, now);
    memset(result, 0, sizeof(*result));
    if (header.type == XGW_MESSAGE_DATA) {
        if (xgw_fec_add_data(&dp->fec, payload, payload_len, result)) {
            snprintf(summary,
                     summary_len,
                     "ok=assembled type=%s session=%u seq=%llu recovered=%s payload_len=%zu packet_len=%zu",
                     xgw_message_type_name(header.type),
                     session->id,
                     (unsigned long long) header.sequence,
                     result->recovered ? "true" : "false",
                     payload_len,
                     result->packet_len);
        } else {
            snprintf(summary,
                     summary_len,
                     "ok=fragment_buffered type=%s session=%u seq=%llu payload_len=%zu",
                     xgw_message_type_name(header.type),
                     session->id,
                     (unsigned long long) header.sequence,
                     payload_len);
        }
        return 1;
    }
    if (header.type == XGW_MESSAGE_FEC) {
        if (xgw_fec_add_parity(&dp->fec, payload, payload_len, result)) {
            snprintf(summary,
                     summary_len,
                     "ok=fec_recovered type=%s session=%u seq=%llu payload_len=%zu packet_len=%zu",
                     xgw_message_type_name(header.type),
                     session->id,
                     (unsigned long long) header.sequence,
                     payload_len,
                     result->packet_len);
        } else {
            snprintf(summary,
                     summary_len,
                     "ok=fec_buffered type=%s session=%u seq=%llu payload_len=%zu",
                     xgw_message_type_name(header.type),
                     session->id,
                     (unsigned long long) header.sequence,
                     payload_len);
        }
        return 1;
    }
    snprintf(summary,
             summary_len,
             "ok=control_frame type=%s session=%u seq=%llu payload_len=%zu",
             xgw_message_type_name(header.type),
             session->id,
             (unsigned long long) header.sequence,
             payload_len);
    return 1;
}

/* 根据明文 payload 构建一组出站帧。 */
int xgw_dataplane_build_outbound(xgw_dataplane_t *dp, xgw_session_t *session, const uint8_t *payload, size_t payload_len, xgw_frame_batch_t *batch, uint8_t *fec_frame, size_t fec_cap, size_t *fec_len) {
    uint64_t next_seq;
    if (strcmp(dp->config.acl_mode, "reject") == 0) {
        set_last_error("acl_reject");
        return 0;
    }
    if (session == NULL) {
        set_last_error("session_null");
        return 0;
    }
    next_seq = session->next_tx_seq;
    if (!xgw_build_data_frames(1U, session->id, next_seq, payload, payload_len, dp->config.profile.payload_size, batch)) {
        set_last_error("build_data_frames_failed");
        return 0;
    }
    session->next_tx_seq = batch->last_sequence + 1U;
    if (dp->fec.enabled) {
        if (!xgw_build_fec_frame(1U, session->id, session->next_tx_seq, batch->group_id, batch, fec_frame, fec_cap, fec_len)) {
            set_last_error("build_fec_frame_failed");
            return 0;
        }
        ++session->next_tx_seq;
    } else if (fec_len != NULL) {
        *fec_len = 0;
    }
    set_last_error("ok");
    return 1;
}
