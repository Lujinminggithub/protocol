/* 数据面核心处理：入站解析、认证校验、控制面、FEC 重组与出站构建。 */

#define _POSIX_C_SOURCE 200809L

#include "xgw_dataplane.h"

#include "xgw_control.h"

#include "log4c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* 控制面调试日志（session.debug/control.before/control.after）按 XGW_RT_VERBOSE
 * 环境变量门控。这些日志在每个控制帧(ack/keepalive)处理时触发，relay 转发量大时
 * 高频刷 stdout，曾把 relay 日志撑到 10GB+、CPU 80%。dataplane.c 不持有 runtime 的
 * 日志级别，故用环境变量惰性读取。 */
static int dataplane_verbose_log(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("XGW_RT_VERBOSE");
        cached = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

static char g_xgw_dataplane_error[128];

static int emit_immediate_feedback_frame(xgw_session_t *session,
                                         xgw_reassembly_result_t *result,
                                         uint64_t timestamp_us);
static int emit_feedback_frame_with_hint(xgw_session_t *session,
                                         xgw_reassembly_result_t *result,
                                         uint64_t timestamp_us,
                                         const xgw_flow_hint_t *hint);

static void set_last_error(const char *text) {
    snprintf(g_xgw_dataplane_error, sizeof(g_xgw_dataplane_error), "%s", text == NULL ? "" : text);
}

static uint64_t now_us(void) {
#ifdef _WIN32
    return (uint64_t) GetTickCount64() * 1000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000ULL) + ((uint64_t) ts.tv_nsec / 1000ULL);
#endif
}

static int emit_feedback_frame_with_hint(xgw_session_t *session,
                                         xgw_reassembly_result_t *result,
                                         uint64_t timestamp_us,
                                         const xgw_flow_hint_t *hint) {
    xgw_ack_info_t ack;
    xgw_keepalive_payload_t feedback;
    xgw_header_t header;
    uint8_t payload_buf[256];
    size_t payload_len;
    size_t out_len = 0U;
    if (session == NULL || result == NULL) {
        return 1;
    }
    if (!xgw_session_flush_feedback_hint(session, timestamp_us, hint, &ack)) {
        return 1;
    }
    xgw_control_make_ack(&ack, timestamp_us, &feedback);
    payload_len = xgw_encode_keepalive_payload(&feedback, payload_buf, sizeof(payload_buf));
    if (payload_len == 0U) {
        return 0;
    }
    memset(&header, 0, sizeof(header));
    header.version = 1U;
    header.type = XGW_MESSAGE_ACK;
    header.flags = XGW_FLAG_CONTROL;
    header.session_id = session->id;
    header.sequence = session->next_tx_seq++;
    header.payload_length = (uint16_t) payload_len;
    if (!xgw_header_encode(&header,
                           payload_buf,
                           payload_len,
                           result->control_frame,
                           sizeof(result->control_frame),
                           &out_len)) {
        return 0;
    }
    result->control_frame_len = out_len;
    return 1;
}

static int emit_immediate_feedback_frame(xgw_session_t *session,
                                         xgw_reassembly_result_t *result,
                                         uint64_t timestamp_us) {
    xgw_keepalive_payload_t feedback;
    xgw_header_t header;
    uint8_t payload_buf[256];
    size_t payload_len;
    size_t out_len = 0U;
    xgw_ack_info_t ack;
    if (session == NULL || result == NULL) {
        return 1;
    }
    if (session->pending_ack.largest_acked == 0U || session->pending_ack.packets_acked == 0U) {
        return 1;
    }
    ack = session->pending_ack;
    ack.ack_delay_us = 0U;
    ack.latest_rtt_us = 0U;
    session->last_feedback_tx_us = timestamp_us;
    session->ack_pending_since_us = 0U;
    memset(&session->pending_ack, 0, sizeof(session->pending_ack));
    xgw_control_make_ack(&ack, timestamp_us, &feedback);
    payload_len = xgw_encode_keepalive_payload(&feedback, payload_buf, sizeof(payload_buf));
    if (payload_len == 0U) {
        return 0;
    }
    memset(&header, 0, sizeof(header));
    header.version = 1U;
    header.type = XGW_MESSAGE_ACK;
    header.flags = XGW_FLAG_CONTROL;
    header.session_id = session->id;
    header.sequence = session->next_tx_seq++;
    header.payload_length = (uint16_t) payload_len;
    if (!xgw_header_encode(&header,
                           payload_buf,
                           payload_len,
                           result->control_frame,
                           sizeof(result->control_frame),
                           &out_len)) {
        return 0;
    }
    result->control_frame_len = out_len;
    return 1;
}

/* 从 bridge 业务负载解析 stream_id(格式:magic "XGB1" + be32 stream_id + ...)。
 * per-stream 多路复用:发送侧据此为每条业务流取独立的传输窗口/序号。
 * 解析失败(非 bridge 帧/控制帧)返回 0,调用方回退到 session 级(stream_id=0)。 */
#define XGW_BRIDGE_PAYLOAD_MAGIC 0x58474231U
static uint32_t parse_stream_id_from_payload(const uint8_t *payload, size_t payload_len) {
    uint32_t magic;
    if (payload == NULL || payload_len < 8U) {
        return 0U;
    }
    magic = ((uint32_t) payload[0] << 24) | ((uint32_t) payload[1] << 16) |
            ((uint32_t) payload[2] << 8) | (uint32_t) payload[3];
    if (magic != XGW_BRIDGE_PAYLOAD_MAGIC) {
        return 0U;
    }
    return ((uint32_t) payload[4] << 24) | ((uint32_t) payload[5] << 16) |
           ((uint32_t) payload[6] << 8) | (uint32_t) payload[7];
}

/* 加密 nonce 构造:per-stream 序号空间下,不同 stream 可能有相同 sequence,
 * 直接用 sequence 作 nonce 会在同一 session(共享 tx_key)内 nonce 复用 → 加密灾难。
 * 把 stream_id 混入高位:nonce = (stream_id<<32) ^ sequence。stream_id 不同则 nonce 不同;
 * 同 stream 内 sequence 单调递增(<2^32 不回绕)→ 全局唯一。收发对称(header 同时带二者)。 */
static uint64_t xgw_frame_nonce(uint32_t stream_id, uint64_t sequence) {
    return ((uint64_t) stream_id << 32) ^ sequence;
}


/* P0 污染修复：收包 ACK/FEC 决策不再读发送残留的 dp->current_flow_hint，
 * 改为按接收会话的 slot 构造中性、确定性的 hint（零跨流污染，测量基线可重复）。
 * 阈值：CONTROL/bootstrap cadence≈3ms+fast_ack；MEDIA≈6ms；BULK≈22ms。
 * per-stream/per-session 自适应留待 P1/P2。 */
static xgw_flow_hint_t neutral_rx_hint_for_slot(xgw_session_slot_t slot) {
    xgw_flow_hint_t h;
    memset(&h, 0, sizeof(h));
    h.valid = 1U;
    switch (slot) {
        case XGW_SESSION_SLOT_CONTROL:
            h.priority = XGW_FLOW_PRIORITY_INTERACTIVE;
            h.budget_flags |= XGW_FLOW_BUDGET_FAST_ACK;
            h.runtime_feedback_cadence_ms = 3U;
            break;
        case XGW_SESSION_SLOT_MEDIA:
            h.priority = XGW_FLOW_PRIORITY_INTERACTIVE;
            h.runtime_feedback_cadence_ms = 6U;
            break;
        case XGW_SESSION_SLOT_BULK:
        default:
            h.runtime_feedback_cadence_ms = 22U;
            break;
    }
    return h;
}

static uint8_t should_emit_immediate_ack(const xgw_flow_hint_t *hint) {
    if (hint == NULL || !hint->valid) {
        return 0U;
    }
    if ((hint->budget_flags & XGW_FLOW_BUDGET_FAST_ACK) != 0U) {
        return 1U;
    }
    if (hint->runtime_ack_credit_frames > 2U) {
        return 1U;
    }
    if (hint->priority >= XGW_FLOW_PRIORITY_INTERACTIVE &&
        hint->runtime_return_delay_ms > 1500U) {
        return 1U;
    }
    return 0U;
}

const char *xgw_dataplane_last_error(void) {
    return g_xgw_dataplane_error;
}

void xgw_dataplane_set_flow_hint(xgw_dataplane_t *dp, const xgw_flow_hint_t *hint) {
    if (dp == NULL) {
        return;
    }
    memset(&dp->current_flow_hint, 0, sizeof(dp->current_flow_hint));
    if (hint != NULL) {
        dp->current_flow_hint = *hint;
        dp->current_flow_hint.valid = hint->valid != 0U ? hint->valid : 1U;
    }
}

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
    xgw_stream_xport_pool_init(&dp->streams);
    xgw_fec_codec_init(&dp->fec, config->profile.fec_data_shards, config->profile.fec_parity_shards);
    xgw_dos_init(&dp->dos, &config->dos);
    xgw_dataplane_pmtu_init(dp);
}

/* 帧层固定开销：主帧头 + 分片头 + 加封（nonce+tag）。用于 PMTU(IP)→应用负载 换算。 */
#define XGW_PMTU_FRAME_OVERHEAD (XGW_HEADER_SIZE + XGW_FRAGMENT_HEADER_SIZE + 8U + 16U)
/* IPv4(20) + UDP(8) 外层头；保守再留少量余量给可能的 obfs/封装。 */
#define XGW_PMTU_IPUDP_OVERHEAD 28U
#define XGW_PMTU_SAFETY_MARGIN 8U
#define XGW_PMTU_PROBE_INTERVAL_US 30000000ULL /* 30s 向上探测一次 */
#define XGW_PMTU_FLOOR_PAYLOAD 512U

void xgw_dataplane_pmtu_init(xgw_dataplane_t *dp) {
    uint32_t configured;
    if (dp == NULL) {
        return;
    }
    memset(&dp->pmtu, 0, sizeof(dp->pmtu));
    /* disable_path_mtu_discovery 同时控制前端 QUIC 与本传输层 PMTUD。 */
    dp->pmtu.enabled = dp->config.disable_path_mtu_discovery ? 0 : 1;
    configured = dp->config.profile.payload_size;
    if (configured == 0U) {
        configured = 1100U;
    }
    dp->pmtu.overhead_bytes = XGW_PMTU_FRAME_OVERHEAD + XGW_PMTU_IPUDP_OVERHEAD + XGW_PMTU_SAFETY_MARGIN;
    dp->pmtu.ceiling_payload = configured;
    dp->pmtu.floor_payload = XGW_PMTU_FLOOR_PAYLOAD < configured ? XGW_PMTU_FLOOR_PAYLOAD : configured;
    dp->pmtu.effective_payload = configured; /* 乐观从配置值起步，遇阻再回退 */
    dp->pmtu.probe_step = 128U;
}

uint32_t xgw_dataplane_effective_payload(const xgw_dataplane_t *dp) {
    if (dp == NULL) {
        return 1100U;
    }
    if (!dp->pmtu.enabled || dp->pmtu.effective_payload == 0U) {
        return dp->config.profile.payload_size == 0U ? 1100U : dp->config.profile.payload_size;
    }
    return dp->pmtu.effective_payload;
}

void xgw_dataplane_pmtu_note_send(xgw_dataplane_t *dp, int too_big, uint64_t now_us) {
    uint32_t reduced;
    if (dp == NULL || !dp->pmtu.enabled || !too_big) {
        return;
    }
    dp->pmtu.too_big_events++;
    dp->pmtu.last_too_big_us = now_us;
    /* 报文过大：按 3/4 收缩，不低于 floor，并推迟下一次向上探测。 */
    reduced = (dp->pmtu.effective_payload * 3U) / 4U;
    if (reduced < dp->pmtu.floor_payload) {
        reduced = dp->pmtu.floor_payload;
    }
    if (reduced < dp->pmtu.effective_payload) {
        printf("pmtu.shrink effective=%u->%u floor=%u ceiling=%u events=%u\n",
               dp->pmtu.effective_payload, reduced, dp->pmtu.floor_payload,
               dp->pmtu.ceiling_payload, dp->pmtu.too_big_events);
        fflush(stdout);
        dp->pmtu.effective_payload = reduced;
    }
    dp->pmtu.last_probe_us = now_us; /* 收缩后重新计时，避免立刻又涨回去 */
}

void xgw_dataplane_pmtu_tick(xgw_dataplane_t *dp, uint32_t kernel_pmtu, uint64_t now_us) {
    if (dp == NULL || !dp->pmtu.enabled) {
        return;
    }
    /* 内核已发现路径 MTU（含 IP/UDP）：换算成应用负载并钳制有效值（取更小者，保守）。 */
    if (kernel_pmtu > (XGW_PMTU_IPUDP_OVERHEAD + XGW_PMTU_FRAME_OVERHEAD + XGW_PMTU_SAFETY_MARGIN)) {
        uint32_t kernel_payload = kernel_pmtu - XGW_PMTU_IPUDP_OVERHEAD - XGW_PMTU_FRAME_OVERHEAD - XGW_PMTU_SAFETY_MARGIN;
        if (kernel_payload < dp->pmtu.floor_payload) {
            kernel_payload = dp->pmtu.floor_payload;
        }
        if (kernel_payload > dp->pmtu.ceiling_payload) {
            kernel_payload = dp->pmtu.ceiling_payload;
        }
        if (kernel_payload < dp->pmtu.effective_payload) {
            printf("pmtu.kernel_clamp kernel_pmtu=%u effective=%u->%u\n",
                   kernel_pmtu, dp->pmtu.effective_payload, kernel_payload);
            fflush(stdout);
            dp->pmtu.effective_payload = kernel_payload;
            dp->pmtu.last_probe_us = now_us;
        }
    }
    /* 一段时间无「过大」事件，向上探测，逐步逼近 ceiling。 */
    if (dp->pmtu.effective_payload < dp->pmtu.ceiling_payload &&
        (dp->pmtu.last_probe_us == 0U || now_us - dp->pmtu.last_probe_us >= XGW_PMTU_PROBE_INTERVAL_US) &&
        (dp->pmtu.last_too_big_us == 0U || now_us - dp->pmtu.last_too_big_us >= XGW_PMTU_PROBE_INTERVAL_US)) {
        uint32_t probed = dp->pmtu.effective_payload + dp->pmtu.probe_step;
        if (probed > dp->pmtu.ceiling_payload) {
            probed = dp->pmtu.ceiling_payload;
        }
        printf("pmtu.probe_up effective=%u->%u ceiling=%u\n",
               dp->pmtu.effective_payload, probed, dp->pmtu.ceiling_payload);
        fflush(stdout);
        dp->pmtu.effective_payload = probed;
        dp->pmtu.last_probe_us = now_us;
    }
}

static void init_session_runtime(xgw_dataplane_t *dp, xgw_session_t *session) {
    xgw_security_init(&session->security, dp->config.auth_token);
    xgw_cc_init(&session->cc,
                dp->config.congestion_mode,
                dp->config.bbr_profile,
                dp->config.advertised_tx_bps,
                dp->config.profile.pacing_rate_bps,
                dp->config.profile.mtu == 0U ? 1200U : dp->config.profile.mtu);
}

static uint32_t select_fec_parity_count(const xgw_dataplane_t *dp,
                                        xgw_session_t *session,
                                        const xgw_frame_batch_t *batch,
                                        const char **reason) {
    uint32_t configured;
    uint32_t selected;
    uint64_t inflight_pct = 0U;
    if (reason != NULL) {
        *reason = "configured";
    }
    if (dp == NULL) {
        return 0U;
    }
    configured = dp->config.profile.fec_parity_shards;
    if (configured == 0U || session == NULL || batch == NULL) {
        return configured;
    }
    if (dp->current_flow_hint.valid) {
        if (dp->current_flow_hint.flow_class == 2U) {
            if (reason != NULL) {
                *reason = "control_plane_low_latency";
            }
            return configured > 0U ? 1U : 0U;
        }
        if ((dp->current_flow_hint.flow_class == 3U || dp->current_flow_hint.flow_class == 8U) && configured < 2U) {
            configured = 2U;
            if (reason != NULL) {
                *reason = dp->current_flow_hint.flow_class == 8U ? "media_bootstrap_floor" : "media_floor";
            }
        }
    }
    if (dp->current_flow_hint.valid &&
        (dp->current_flow_hint.budget_flags & XGW_FLOW_BUDGET_REDUCED_FEC) != 0U) {
        if (reason != NULL) {
            *reason = "flow_hint_reduced_fec";
        }
        configured = configured > 0U ? 1U : 0U;
    }
    if (dp->current_flow_hint.valid) {
        if (dp->current_flow_hint.runtime_ack_credit_frames > 2U) {
            if (reason != NULL) {
                *reason = "ack_debt_guard";
            }
            return configured > 0U ? 1U : 0U;
        }
        if (dp->current_flow_hint.runtime_parity_budget > 0U) {
            if (reason != NULL) {
                *reason = "parity_budget";
            }
            configured = dp->current_flow_hint.runtime_parity_budget;
        }
        if (dp->current_flow_hint.runtime_return_delay_ms > 4000U &&
            dp->current_flow_hint.runtime_inflight_bytes > 128U * 1024U) {
            if (reason != NULL) {
                *reason = "return_delay_pressure";
            }
            return configured > 1U ? 1U : configured;
        }
        if (dp->current_flow_hint.runtime_starvation_boost > 3U && configured < 2U) {
            if (reason != NULL) {
                *reason = "starvation_protect";
            }
            configured = 2U;
        }
    }
    if (batch->data_frame_count <= 1U) {
        if (reason != NULL) {
            *reason = "single_fragment";
        }
        return 0U;
    }
    if (session->cc.mode != XGW_CC_BBR) {
        return configured;
    }
    selected = configured;
    if (session->cc.cwnd_bytes > 0U) {
        inflight_pct = (session->cc.inflight_bytes * 100ULL) / session->cc.cwnd_bytes;
    }
    if (session->cc.bbr_mode == XGW_BBR_PROBE_RTT) {
        if (reason != NULL) {
            *reason = "probe_rtt";
        }
        return 0U;
    }
    if (session->cc.recovery_rounds_left > 0U) {
        selected = configured > 1U ? 1U : configured;
        if (reason != NULL) {
            *reason = "recovery_guard";
        }
    } else if (session->cc.min_rtt_us > 0U &&
               session->cc.latest_rtt_us > session->cc.min_rtt_us * 13ULL / 10ULL &&
               inflight_pct >= 85U) {
        selected = configured > 1U ? configured / 2U : 1U;
        if (selected == 0U && configured > 0U) {
            selected = 1U;
        }
        if (reason != NULL) {
            *reason = "queue_pressure";
        }
    } else if (configured > 1U &&
               session->cc.min_rtt_us > 0U &&
               session->cc.latest_rtt_us <= session->cc.min_rtt_us * 11ULL / 10ULL &&
               inflight_pct < 70U) {
        selected = 1U;
        if (reason != NULL) {
            *reason = "steady_low_loss";
        }
    }
    return selected;
}

/* 自愈重握手：当本端收到无法解密的加密帧（对端重启、安全状态已重置），
 * 不再静默丢包，而是把本端控制态回退到 INIT 并构造一个 HELLO 帧放入
 * result->control_frame，由 runtime 回发给来源对端。对端（仍处于 ESTABLISHED）
 * 收到合法 HELLO 后会走 control.c 的重握手分支重建会话。这样无论先重启哪一跳，
 * 链路都能自动恢复，从而去掉“必须按 egress→relay→ingress 顺序重启”的硬性约束，
 * 并满足后续动态选路的需要。
 *
 * 用 last_control_tx_us 做 1s 限速，避免对端持续发来的旧加密包触发 HELLO 风暴。
 * 返回 1 表示已写入 result->control_frame（调用方需保留并回发）。 */
static int emit_rehandshake_hello(xgw_dataplane_t *dp,
                                  xgw_session_t *session,
                                  xgw_reassembly_result_t *result,
                                  uint64_t tick_us) {
    xgw_hello_payload_t hello;
    xgw_header_t out_header;
    uint8_t payload[64];
    size_t payload_len;
    size_t out_len = 0U;
    if (dp == NULL || session == NULL || result == NULL) {
        return 0;
    }
    if (dp->config.auth_token[0] == '\0') {
        return 0;
    }
    /* 1s 限速：刚发过控制帧（HELLO/keepalive 等）则跳过，避免风暴。 */
    if (session->last_control_tx_us != 0U &&
        tick_us > session->last_control_tx_us &&
        tick_us - session->last_control_tx_us < 1000000ULL) {
        return 0;
    }
    /* 回退本端控制态并重置安全状态，准备重新发起握手。 */
    xgw_session_reset_transport_state(session);
    session->control_state = XGW_CTRL_INIT;
    session->security.session_ready = 0;
    session->security.local_nonce = 0U;
    session->security.peer_nonce = 0U;
    xgw_security_begin_handshake(&session->security,
                                 tick_us ^ ((uint64_t) session->id << 16U));
    xgw_control_make_hello(&session->security, &dp->negotiation, dp->config.profile.mtu, &hello);
    payload_len = xgw_encode_hello_payload(&hello, payload, sizeof(payload));
    if (payload_len == 0U) {
        return 0;
    }
    memset(&out_header, 0, sizeof(out_header));
    out_header.version = 1U;
    out_header.type = XGW_MESSAGE_HELLO;
    out_header.flags = XGW_FLAG_CONTROL;
    out_header.session_id = session->id;
    out_header.sequence = session->next_tx_seq++;
    out_header.payload_length = (uint16_t) payload_len;
    if (!xgw_header_encode(&out_header,
                           payload,
                           payload_len,
                           result->control_frame,
                           sizeof(result->control_frame),
                           &out_len)) {
        return 0;
    }
    result->control_frame_len = out_len;
    session->control_state = XGW_CTRL_HELLO_SENT;
    session->last_control_tx_us = tick_us;
    printf("session.rehandshake.tx remote=%s:%u id=%u reason=secure_open_fail nonce=%llu\n",
           session->remote_addr.host,
           session->remote_addr.port,
           session->id,
           (unsigned long long) session->security.local_nonce);
    fflush(stdout);
    return 1;
}

int xgw_dataplane_process_frame(xgw_dataplane_t *dp, const xgw_packet_t *packet, xgw_reassembly_result_t *result, char *summary, size_t summary_len) {
    xgw_header_t header;
    const uint8_t *payload = NULL;
    size_t payload_len = 0U;
    xgw_session_t *session;
    time_t now = time(NULL);
    const char *reason = NULL;
    uint8_t secure_buf[65535];
    const uint8_t *plain_payload = NULL;
    size_t plain_payload_len = 0U;
    char sec_summary[192];
    uint64_t tick_us = now_us();

    if (!dp->negotiation.udp_enabled) {
        snprintf(summary, summary_len, "drop=udp_disabled remote=%s packet_len=%zu", packet->remote_host, packet->data_len);
        return 0;
    }
    if (!xgw_dos_allow(&dp->dos, packet->remote_host, now, &reason)) {
        snprintf(summary, summary_len, "drop=dos remote=%s reason=%s packet_len=%zu",
                 packet->remote_host, reason == NULL ? "blocked" : reason, packet->data_len);
        return 0;
    }
    if (!xgw_header_decode(packet->data, packet->data_len, &header, &payload, &payload_len)) {
        snprintf(summary, summary_len, "drop=header_decode_fail remote=%s packet_len=%zu", packet->remote_host, packet->data_len);
        return 0;
    }
    result->session_id = header.session_id;
    session = xgw_session_upsert(&dp->sessions,
                                 header.session_id,
                                 dp->config.allow_policy.group_id,
                                 "",
                                 packet->remote_host,
                                 packet->remote_port,
                                 now);
    if (session == NULL) {
        snprintf(summary, summary_len, "drop=session_table_full type=%s session=%u seq=%llu",
                 xgw_message_type_name(header.type), header.session_id, (unsigned long long) header.sequence);
        return 0;
    }
    if (session->security.static_ready == 0 && dp->config.auth_token[0] != '\0') {
        init_session_runtime(dp, session);
    }
    xgw_security_debug_summary(&session->security, sec_summary, sizeof(sec_summary));
    if (dataplane_verbose_log()) {
    printf("session.debug remote=%s:%u id=%u state=%d %s\n",
           session->remote_addr.host,
           session->remote_addr.port,
           session->id,
           session->control_state,
           sec_summary);
    fflush(stdout);
    }
    if (header.type != XGW_MESSAGE_HELLO &&
        header.type != XGW_MESSAGE_HELLO_ACK &&
        header.type != XGW_MESSAGE_CONFIRM) {
        /* per-stream 多路复用:DATA/FEC 帧(stream_id≠0)用该流独立的 replay window,
         * 避免不同 stream 的 per-stream 序号互相误判重放;控制帧仍用 session 级 window。 */
        xgw_replay_window_t *rw = &session->replay_window;
        if (header.stream_id != 0U) {
            xgw_stream_xport_t *rx_xport = xgw_stream_xport_get(&dp->streams, session->id, header.stream_id, 1);
            if (rx_xport != NULL) {
                rw = &rx_xport->replay_window;
                rx_xport->last_activity_us = tick_us;
            }
        }
        if (!xgw_replay_window_accept(rw, header.sequence, dp->config.profile.reorder_window)) {
            snprintf(summary, summary_len, "drop=replay type=%s session=%u stream=%u seq=%llu",
                     xgw_message_type_name(header.type), session->id, header.stream_id, (unsigned long long) header.sequence);
            return 0;
        }
    }
    xgw_session_touch(session, now);
    memset(result, 0, sizeof(*result));

    if (header.type == XGW_MESSAGE_HELLO ||
        header.type == XGW_MESSAGE_HELLO_ACK ||
        header.type == XGW_MESSAGE_CONFIRM ||
        header.type == XGW_MESSAGE_KEEPALIVE ||
        header.type == XGW_MESSAGE_ACK) {
        xgw_control_result_t ctrl;
        if (dataplane_verbose_log()) {
        printf("control.before type=%s remote=%s:%u id=%u state=%d %s\n",
               xgw_message_type_name(header.type),
               session->remote_addr.host,
               session->remote_addr.port,
               session->id,
               session->control_state,
               sec_summary);
        fflush(stdout);
        }
        if (!xgw_control_process(header.type,
                                 payload,
                                 payload_len,
                                 &session->security,
                                 &session->control_state,
                                 &dp->negotiation,
                                 dp->config.profile.mtu,
                                 &ctrl)) {
            snprintf(summary, summary_len, "drop=control_auth_fail type=%s session=%u seq=%llu",
                     xgw_message_type_name(header.type), session->id, (unsigned long long) header.sequence);
            return 0;
        }
        xgw_security_debug_summary(&session->security, sec_summary, sizeof(sec_summary));
        if (dataplane_verbose_log()) {
        printf("control.after type=%s remote=%s:%u id=%u state=%d established=%d %s\n",
               xgw_message_type_name(header.type),
               session->remote_addr.host,
               session->remote_addr.port,
               session->id,
               session->control_state,
               ctrl.established,
               sec_summary);
        fflush(stdout);
        }
        session->last_control_rx_us = tick_us;
        xgw_session_note_rx(session, header.sequence, packet->data_len, tick_us);
        if (ctrl.reset_transport_state) {
            xgw_session_reset_transport_state(session);
            printf("session.transport.reset remote=%s:%u id=%u state=%d reason=handshake_refresh\n",
                   session->remote_addr.host,
                   session->remote_addr.port,
                   session->id,
                   session->control_state);
            fflush(stdout);
        }
        if (ctrl.ack.largest_acked > 0U) {
            session->latest_peer_timestamp_us = ctrl.peer_timestamp_us;
            xgw_session_mark_acked(session, &ctrl.ack, tick_us);
            xgw_session_mark_loss_before(session, ctrl.ack.largest_acked, tick_us);
        }
        if (ctrl.emit_response) {
            xgw_header_t out_header;
            size_t out_len = 0U;
            memset(&out_header, 0, sizeof(out_header));
            out_header.version = 1U;
            out_header.type = ctrl.response_type;
            out_header.flags = ctrl.response_flags;
            out_header.session_id = session->id;
            out_header.sequence = session->next_tx_seq++;
            out_header.payload_length = (uint16_t) ctrl.response_payload_len;
            if (!xgw_header_encode(&out_header,
                                   ctrl.response_payload,
                                   ctrl.response_payload_len,
                                   result->control_frame,
                                   sizeof(result->control_frame),
                                   &out_len)) {
                snprintf(summary, summary_len, "drop=control_encode_fail type=%s session=%u",
                         xgw_message_type_name(ctrl.response_type), session->id);
                return 0;
            }
            result->control_frame_len = out_len;
        }
        snprintf(summary, summary_len, "ok=control type=%s session=%u state=%d established=%d response=%zu",
                 xgw_message_type_name(header.type), session->id, session->control_state, ctrl.established, result->control_frame_len);
        return 1;
    }

    plain_payload = payload;
    plain_payload_len = payload_len;
    if ((header.flags & XGW_FLAG_SECURE) != 0U) {
        plain_payload_len = xgw_security_open_payload(&session->security,
                                                      payload,
                                                      payload_len,
                                                      xgw_frame_nonce(header.stream_id, header.sequence),
                                                      secure_buf,
                                                      sizeof(secure_buf));
        if (plain_payload_len == 0U) {
            printf("session.secure_open_fail remote=%s:%u id=%u state=%d seq=%llu %s\n",
                   session->remote_addr.host,
                   session->remote_addr.port,
                   session->id,
                   session->control_state,
                   (unsigned long long) header.sequence,
                   sec_summary);
            fflush(stdout);
            /* 对端安全状态与本端不一致（通常是对端重启后重置了会话）。
             * 主动回发 HELLO 触发重握手，使链路自愈、不再依赖重启顺序。 */
            if (emit_rehandshake_hello(dp, session, result, tick_us)) {
                snprintf(summary, summary_len,
                         "drop=secure_open_fail.rehandshake type=%s session=%u seq=%llu",
                         xgw_message_type_name(header.type), session->id,
                         (unsigned long long) header.sequence);
            } else {
                snprintf(summary, summary_len, "drop=secure_open_fail type=%s session=%u seq=%llu",
                         xgw_message_type_name(header.type), session->id,
                         (unsigned long long) header.sequence);
            }
            return 0;
        }
        plain_payload = secure_buf;
    }

    xgw_session_note_rx(session, header.sequence, packet->data_len, tick_us);

    if (header.type == XGW_MESSAGE_DATA) {
        /* 帧级时间线埋点:回程数据帧到达时刻。带 session_id —— xport 按 (session_id,stream_id) 索引,
         * 同一 stream_id 经不同 slot session(control/media/bulk) 是独立 seq 空间;只记 stream_id 会把
         * 多个独立序号空间合并成一条时间线,造成"seq 倒退"假象。加 session 区分,坐实真伪。 */
        if (header.stream_id != 0U) {
            log4c_info("rtt.frame.recv session=%u stream=%u seq=%llu len=%zu peer=%s:%u",
                       header.session_id, header.stream_id, (unsigned long long) header.sequence,
                       plain_payload_len, session->remote_addr.host, session->remote_addr.port);
        }
        if (xgw_fec_add_data(&dp->fec,
                             session->id,
                             session->remote_addr.host,
                             session->remote_addr.port,
                             plain_payload,
                             plain_payload_len,
                             result)) {
            snprintf(summary, summary_len, "ok=assembled type=%s session=%u seq=%llu recovered=%s payload_len=%zu packet_len=%zu",
                     xgw_message_type_name(header.type), session->id, (unsigned long long) header.sequence,
                     result->recovered ? "true" : "false", plain_payload_len, result->packet_len);
        } else {
            snprintf(summary, summary_len, "ok=fragment_buffered type=%s session=%u peer=%s:%u seq=%llu payload_len=%zu",
                     xgw_message_type_name(header.type), session->id, session->remote_addr.host, session->remote_addr.port,
                     (unsigned long long) header.sequence, plain_payload_len);
        }
        if (dataplane_verbose_log()) {
        printf("dataplane.egress.data remote=%s:%u id=%u state=%d result_packet_len=%zu recovered=%d payload_len=%zu\n",
               session->remote_addr.host,
               session->remote_addr.port,
               session->id,
               session->control_state,
               result->packet_len,
               result->recovered,
               plain_payload_len);
        fflush(stdout);
        }
        if (session->control_state == XGW_CTRL_ESTABLISHED) {
            xgw_flow_hint_t rx_hint = neutral_rx_hint_for_slot(session->scope.slot);
            if (!(should_emit_immediate_ack(&rx_hint)
                        ? emit_immediate_feedback_frame(session, result, tick_us)
                        : emit_feedback_frame_with_hint(session, result, tick_us, &rx_hint))) {
                snprintf(summary, summary_len, "drop=ack_encode_fail type=%s session=%u seq=%llu",
                         xgw_message_type_name(header.type), session->id, (unsigned long long) header.sequence);
                return 0;
            }
        }
        return 1;
    }
    if (header.type == XGW_MESSAGE_FEC) {
        if (xgw_fec_add_parity(&dp->fec,
                               session->id,
                               session->remote_addr.host,
                               session->remote_addr.port,
                               plain_payload,
                               plain_payload_len,
                               result)) {
            snprintf(summary, summary_len, "ok=fec_recovered type=%s session=%u seq=%llu payload_len=%zu packet_len=%zu",
                     xgw_message_type_name(header.type), session->id, (unsigned long long) header.sequence,
                     plain_payload_len, result->packet_len);
        } else {
            snprintf(summary, summary_len, "ok=fec_buffered type=%s session=%u peer=%s:%u seq=%llu payload_len=%zu",
                     xgw_message_type_name(header.type), session->id, session->remote_addr.host, session->remote_addr.port,
                     (unsigned long long) header.sequence, plain_payload_len);
        }
        if (dataplane_verbose_log()) {
        printf("dataplane.egress.fec remote=%s:%u id=%u state=%d result_packet_len=%zu recovered=%d payload_len=%zu\n",
               session->remote_addr.host,
               session->remote_addr.port,
               session->id,
               session->control_state,
               result->packet_len,
               result->recovered,
               plain_payload_len);
        fflush(stdout);
        }
        if (session->control_state == XGW_CTRL_ESTABLISHED) {
            xgw_flow_hint_t rx_hint = neutral_rx_hint_for_slot(session->scope.slot);
            if (!(should_emit_immediate_ack(&rx_hint)
                        ? emit_immediate_feedback_frame(session, result, tick_us)
                        : emit_feedback_frame_with_hint(session, result, tick_us, &rx_hint))) {
                snprintf(summary, summary_len, "drop=ack_encode_fail type=%s session=%u seq=%llu",
                         xgw_message_type_name(header.type), session->id, (unsigned long long) header.sequence);
                return 0;
            }
        }
        return 1;
    }
    snprintf(summary, summary_len, "drop=unknown_type type=%s session=%u seq=%llu",
             xgw_message_type_name(header.type), session->id, (unsigned long long) header.sequence);
    return 0;
}

int xgw_dataplane_build_outbound(xgw_dataplane_t *dp, xgw_session_t *session, const uint8_t *payload, size_t payload_len, xgw_frame_batch_t *batch, uint8_t *fec_frame, size_t fec_cap, size_t *fec_len) {
    uint64_t next_seq;
    uint16_t flags = XGW_FLAG_ACK_ELICITING;
    size_t parity_count = 0U;
    size_t fec_count = 0U;
    uint64_t last_fec_seq = 0U;
    size_t i;
    const char *fec_reason = "configured";
    uint32_t stream_id;
    xgw_stream_xport_t *xport = NULL;

    if (strcmp(dp->config.acl_mode, "reject") == 0) {
        set_last_error("acl_reject");
        return 0;
    }
    if (session == NULL || batch == NULL || payload == NULL) {
        set_last_error("session_null");
        return 0;
    }
	    if (session->control_state != XGW_CTRL_ESTABLISHED && dp->config.auth_token[0] != '\0') {
	        set_last_error("session_not_established");
	        return 0;
	    }

        /* per-stream 多路复用:解析业务流 stream_id,取该流独立的传输窗口/序号空间。
         * 序号 per-stream 独立 → 对端按 (session_id, stream_id) 重组,慢流不阻塞同 slot 其他流。
         * stream_id=0(控制帧/非 bridge 帧)回退到 session 级序号,保持兼容。 */
        stream_id = parse_stream_id_from_payload(payload, payload_len);
        if (stream_id != 0U) {
            xport = xgw_stream_xport_get(&dp->streams, session->id, stream_id, 1);
        }
        next_seq = xport != NULL ? xport->next_tx_seq : session->next_tx_seq;
        memset(&batch->flow_hint, 0, sizeof(batch->flow_hint));
        if (dp->current_flow_hint.valid) {
            batch->flow_hint = dp->current_flow_hint;
        }
	    if (!xgw_build_data_frames(XGW_PROTOCOL_VERSION,
	                               session->id,
                               stream_id,
                               next_seq,
                               payload,
                               payload_len,
                               xgw_dataplane_effective_payload(dp),
                               flags,
                               batch)) {
        set_last_error("build_data_frames_failed");
        return 0;
    }
    if (xport != NULL) {
        xport->next_tx_seq = batch->last_sequence + 1U;
    } else {
        session->next_tx_seq = batch->last_sequence + 1U;
    }
    if (dp->fec.enabled) {
        parity_count = select_fec_parity_count(dp, session, batch, &fec_reason);
        if (session->last_fec_parity_used != parity_count) {
            printf("fec.adapt session=%u peer=%s:%u configured=%u selected=%zu reason=%s bbr_mode=%s inflight=%llu cwnd=%llu latest_rtt=%llu min_rtt=%llu\n",
                   session->id,
                   session->remote_addr.host,
                   session->remote_addr.port,
                   dp->config.profile.fec_parity_shards,
                   parity_count,
                   fec_reason == NULL ? "configured" : fec_reason,
                   xgw_bbr_mode_name(session->cc.bbr_mode),
                   (unsigned long long) session->cc.inflight_bytes,
                   (unsigned long long) session->cc.cwnd_bytes,
                   (unsigned long long) session->cc.latest_rtt_us,
                   (unsigned long long) session->cc.min_rtt_us);
            fflush(stdout);
            session->last_fec_parity_used = (uint32_t) parity_count;
        }
        if (!xgw_build_fec_frames(XGW_PROTOCOL_VERSION,
                                  session->id,
                                  stream_id,
                                  xport != NULL ? xport->next_tx_seq : session->next_tx_seq,
                                  batch,
                                  (uint32_t) parity_count,
                                  flags,
                                  &batch->frames[batch->data_frame_count],
                                  &batch->frame_lengths[batch->data_frame_count],
                                  XGW_MAX_FEC_PARITY,
                                  &fec_count,
                                  &last_fec_seq)) {
            set_last_error("build_fec_frames_failed");
            return 0;
        }
        batch->fec_frame_count = fec_count;
        batch->frame_count = batch->data_frame_count + fec_count;
        if (xport != NULL) {
            xport->next_tx_seq = last_fec_seq + 1U;
        } else {
            session->next_tx_seq = last_fec_seq + 1U;
        }
        if (fec_len != NULL) {
            if (fec_count > 0U) {
                if (fec_cap < batch->frame_lengths[batch->data_frame_count]) {
                    set_last_error("fec_buffer_too_small");
                    return 0;
                }
                memcpy(fec_frame, batch->frames[batch->data_frame_count], batch->frame_lengths[batch->data_frame_count]);
                *fec_len = batch->frame_lengths[batch->data_frame_count];
            } else {
                *fec_len = 0U;
            }
        }
    } else if (fec_len != NULL) {
        *fec_len = 0U;
    }

    if (session->security.session_ready) {
        for (i = 0U; i < batch->frame_count; ++i) {
            xgw_header_t header;
            const uint8_t *plain_payload = NULL;
            size_t plain_payload_len = 0U;
            uint8_t sealed_payload[65535];
            size_t sealed_payload_len;
            size_t out_len = 0U;
            if (!xgw_header_decode(batch->frames[i],
                                   batch->frame_lengths[i],
                                   &header,
                                   &plain_payload,
                                   &plain_payload_len)) {
                set_last_error("secure_header_decode_failed");
                return 0;
            }
            sealed_payload_len = xgw_security_seal_payload(&session->security,
                                                           plain_payload,
                                                           plain_payload_len,
                                                           xgw_frame_nonce(header.stream_id, header.sequence),
                                                           sealed_payload,
                                                           sizeof(sealed_payload));
            if (sealed_payload_len == 0U) {
                set_last_error("secure_seal_failed");
                return 0;
            }
            header.flags |= XGW_FLAG_SECURE;
            header.payload_length = (uint16_t) sealed_payload_len;
            if (!xgw_header_encode(&header,
                                   sealed_payload,
                                   sealed_payload_len,
                                   batch->frames[i],
                                   sizeof(batch->frames[i]),
                                   &out_len)) {
                set_last_error("secure_header_encode_failed");
                return 0;
            }
            batch->frame_lengths[i] = out_len;
        }
        if (fec_len != NULL && batch->fec_frame_count > 0U) {
            memcpy(fec_frame,
                   batch->frames[batch->data_frame_count],
                   batch->frame_lengths[batch->data_frame_count]);
            *fec_len = batch->frame_lengths[batch->data_frame_count];
        }
    }

    set_last_error("ok");
    return 1;
}
