/* Session、replay window、ACK 反馈与发送采样管理实现。 */

#include "xgw_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 控制面 upsert 日志按 XGW_RT_VERBOSE 环境变量门控（与 runtime 日志级别一致）。
 * session.c 不持有 runtime 的 g_xgw_log_level，故用环境变量惰性读取，避免每个
 * 主循环迭代都同步 printf+fflush 把 ingress 日志撑爆、CPU 打满。 */
static int session_verbose_log(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("XGW_RT_VERBOSE");
        cached = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

static uint64_t max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

const char *xgw_transport_segment_name(xgw_transport_segment_t segment) {
    switch (segment) {
        case XGW_TRANSPORT_SEGMENT_MOBILE_INGRESS:
            return "mobile-ingress";
        case XGW_TRANSPORT_SEGMENT_INGRESS_RELAY:
            return "ingress-relay";
        case XGW_TRANSPORT_SEGMENT_RELAY_EGRESS:
            return "relay-egress";
        default:
            return "unknown";
    }
}

void xgw_transport_scope_set(xgw_transport_scope_t *scope,
                             const char *line_id,
                             xgw_transport_segment_t segment,
                             xgw_session_slot_t slot,
                             const char *peer_host,
                             uint16_t peer_port) {
    if (scope == NULL) {
        return;
    }
    memset(scope, 0, sizeof(*scope));
    snprintf(scope->line_id, sizeof(scope->line_id), "%s", line_id == NULL ? "" : line_id);
    scope->segment = segment;
    scope->slot = slot;
    snprintf(scope->peer_host, sizeof(scope->peer_host), "%s", peer_host == NULL ? "" : peer_host);
    scope->peer_port = peer_port;
}

const char *xgw_transport_scope_summary(const xgw_transport_scope_t *scope,
                                        char *buf,
                                        size_t buf_len) {
    char line_id[33];
    char peer_host[33];
    if (buf == NULL || buf_len == 0U) {
        return "";
    }
    if (scope == NULL) {
        snprintf(buf, buf_len, "scope=null");
        return buf;
    }
    snprintf(line_id, sizeof(line_id), "%.32s", scope->line_id);
    snprintf(peer_host, sizeof(peer_host), "%.32s", scope->peer_host);
    snprintf(buf,
             buf_len,
             "line=%s seg=%s slot=%d peer=%s:%u",
             line_id,
             xgw_transport_segment_name(scope->segment),
             (int) scope->slot,
             peer_host,
             scope->peer_port);
    return buf;
}

static uint32_t session_feedback_packet_threshold(const xgw_flow_hint_t *hint) {
    if (hint != NULL && hint->valid) {
        if (hint->flow_class == 3U) {
            return 4U;
        }
        if (hint->flow_class == 2U) {
            return 1U;
        }
        if ((hint->budget_flags & XGW_FLOW_BUDGET_FAST_ACK) != 0U) {
            return 1U;
        }
        if (hint->runtime_ack_credit_frames >= 4U) {
            return 1U;
        }
        if (hint->priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
            return 1U;
        }
    }
    return 2U;
}

static uint32_t session_feedback_bytes_threshold(const xgw_flow_hint_t *hint) {
    if (hint != NULL && hint->valid) {
        if (hint->flow_class == 3U) {
            return 8192U;
        }
        if (hint->flow_class == 2U) {
            return 1024U;
        }
        if ((hint->budget_flags & XGW_FLOW_BUDGET_FAST_ACK) != 0U) {
            return 1200U;
        }
        if (hint->priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
            return 2048U;
        }
        if (hint->runtime_ack_credit_frames >= 4U) {
            return 1500U;
        }
    }
    return 4096U;
}

static uint64_t session_feedback_cadence_us(const xgw_flow_hint_t *hint) {
    uint64_t cadence_us = 25000ULL;
    if (hint != NULL && hint->valid) {
        if (hint->flow_class == 3U) {
            cadence_us = 20000ULL;
        } else if (hint->flow_class == 2U) {
            cadence_us = 3000ULL;
        } else
        if (hint->runtime_feedback_cadence_ms > 0U) {
            cadence_us = (uint64_t) hint->runtime_feedback_cadence_ms * 1000ULL;
        } else if ((hint->budget_flags & XGW_FLOW_BUDGET_FAST_ACK) != 0U) {
            cadence_us = 4000ULL;
        } else if (hint->priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
            cadence_us = 8000ULL;
        } else if (hint->runtime_ack_credit_frames >= 4U) {
            cadence_us = 6000ULL;
        }
    }
    if (cadence_us < 2000ULL) {
        cadence_us = 2000ULL;
    }
    if (cadence_us > 50000ULL) {
        cadence_us = 50000ULL;
    }
    return cadence_us;
}

static void replay_mark(xgw_replay_window_t *window, uint64_t delta) {
    size_t word = (size_t) (delta / 64U);
    uint32_t bit = (uint32_t) (delta % 64U);
    if (word < XGW_REPLAY_BITMAP_WORDS) {
        window->bitmap[word] |= (1ULL << bit);
    }
}

static int replay_is_marked(const xgw_replay_window_t *window, uint64_t delta) {
    size_t word = (size_t) (delta / 64U);
    uint32_t bit = (uint32_t) (delta % 64U);
    if (word >= XGW_REPLAY_BITMAP_WORDS) {
        return 0;
    }
    return (window->bitmap[word] & (1ULL << bit)) != 0;
}

static void replay_shift_left(xgw_replay_window_t *window, uint64_t shift) {
    size_t i;
    size_t words;
    uint32_t bits;
    if (shift >= (uint64_t) (XGW_REPLAY_BITMAP_WORDS * 64U)) {
        memset(window->bitmap, 0, sizeof(window->bitmap));
        return;
    }
    words = (size_t) (shift / 64U);
    bits = (uint32_t) (shift % 64U);
    if (words > 0U) {
        for (i = XGW_REPLAY_BITMAP_WORDS; i > 0U; --i) {
            size_t idx = i - 1U;
            window->bitmap[idx] = idx >= words ? window->bitmap[idx - words] : 0U;
        }
    }
    if (bits > 0U) {
        for (i = XGW_REPLAY_BITMAP_WORDS; i > 0U; --i) {
            size_t idx = i - 1U;
            uint64_t carry = idx > 0U ? window->bitmap[idx - 1U] >> (64U - bits) : 0U;
            window->bitmap[idx] = (window->bitmap[idx] << bits) | carry;
        }
    }
}

void xgw_replay_window_init(xgw_replay_window_t *window) {
    memset(window, 0, sizeof(*window));
}

int xgw_replay_window_accept(xgw_replay_window_t *window, uint64_t seq, uint32_t max_reorder_window) {
    uint64_t delta;
    if (seq == 0U) {
        return 0;
    }
    if (max_reorder_window == 0U) {
        max_reorder_window = XGW_REPLAY_BITMAP_WORDS * 64U;
    }
    if (window->max_seq == 0U) {
        window->max_seq = seq;
        replay_mark(window, 0U);
        return 1;
    }
    if (seq > window->max_seq) {
        replay_shift_left(window, seq - window->max_seq);
        window->max_seq = seq;
        replay_mark(window, 0U);
        return 1;
    }
    delta = window->max_seq - seq;
    if (delta >= max_reorder_window || replay_is_marked(window, delta)) {
        return 0;
    }
    replay_mark(window, delta);
    return 1;
}

void xgw_session_table_init(xgw_session_table_t *table) {
    memset(table, 0, sizeof(*table));
}

xgw_session_t *xgw_session_find_by_id(xgw_session_table_t *table, uint32_t session_id) {
    size_t i;
    for (i = 0; i < table->count; ++i) {
        if (table->sessions[i].active && table->sessions[i].id == session_id) {
            return &table->sessions[i];
        }
    }
    return NULL;
}

xgw_session_t *xgw_session_find_by_peer(xgw_session_table_t *table,
                                        uint32_t session_id,
                                        const char *host,
                                        uint16_t port) {
    size_t i;
    const char *peer_host = host == NULL ? "" : host;
    for (i = 0; i < table->count; ++i) {
        if (!table->sessions[i].active || table->sessions[i].id != session_id) {
            continue;
        }
        if (strcmp(table->sessions[i].remote_addr.host, peer_host) == 0 &&
            table->sessions[i].remote_addr.port == port) {
            return &table->sessions[i];
        }
    }
    return NULL;
}

xgw_session_t *xgw_session_find_by_tunnel_ip(xgw_session_table_t *table, const char *tunnel_ip) {
    size_t i;
    for (i = 0; i < table->count; ++i) {
        if (table->sessions[i].active && strcmp(table->sessions[i].tunnel_ip, tunnel_ip == NULL ? "" : tunnel_ip) == 0) {
            return &table->sessions[i];
        }
    }
    return NULL;
}

xgw_session_t *xgw_session_upsert(xgw_session_table_t *table,
                                  uint32_t session_id,
                                  const char *group_id,
                                  const char *tunnel_ip,
                                  const char *host,
                                  uint16_t port,
                                  time_t now) {
    xgw_session_t *session = xgw_session_find_by_peer(table, session_id, host, port);
    int created = 0;
    if (session == NULL && (host == NULL || host[0] == '\0') && port == 0U) {
        session = xgw_session_find_by_id(table, session_id);
    }
    if (session == NULL) {
        if (table->count >= XGW_MAX_SESSIONS) {
            return NULL;
        }
        session = &table->sessions[table->count++];
        memset(session, 0, sizeof(*session));
        session->id = session_id;
        session->active = 1;
        session->next_tx_seq = 1U;
        session->next_rx_seq = 1U;
        session->control_state = XGW_CTRL_INIT;
        xgw_transport_scope_set(&session->scope,
                                "",
                                XGW_TRANSPORT_SEGMENT_UNKNOWN,
                                XGW_SESSION_SLOT_BULK,
                                "",
                                0U);
        xgw_replay_window_init(&session->replay_window);
        xgw_cc_init(&session->cc, XGW_CC_BBR, XGW_BBR_STANDARD, 0U, 200ULL * 1000ULL * 1000ULL, 1200U);
        created = 1;
    }
    snprintf(session->group_id, sizeof(session->group_id), "%s", group_id == NULL ? "" : group_id);
    snprintf(session->tunnel_ip, sizeof(session->tunnel_ip), "%s", tunnel_ip == NULL ? "" : tunnel_ip);
    snprintf(session->remote_addr.host, sizeof(session->remote_addr.host), "%s", host == NULL ? "" : host);
    session->remote_addr.port = port;
    session->last_seen = now;
    {
        char scope_buf[160];
        if (session_verbose_log()) {
        printf("session.upsert %s id=%u remote=%s:%u state=%d %s table_count=%zu\n",
           created ? "create" : "reuse",
           session->id,
           session->remote_addr.host,
           session->remote_addr.port,
           session->control_state,
           xgw_transport_scope_summary(&session->scope, scope_buf, sizeof(scope_buf)),
           table->count);
        fflush(stdout);
        }
    }
    return session;
}

void xgw_session_set_transport_scope(xgw_session_t *session,
                                     const char *line_id,
                                     xgw_transport_segment_t segment,
                                     xgw_session_slot_t slot,
                                     const char *peer_host,
                                     uint16_t peer_port) {
    if (session == NULL) {
        return;
    }
    xgw_transport_scope_set(&session->scope, line_id, segment, slot, peer_host, peer_port);
}

void xgw_session_touch(xgw_session_t *session, time_t now) {
    if (session != NULL) {
        session->last_seen = now;
    }
}

void xgw_session_record_send(xgw_session_t *session, uint64_t sequence, size_t wire_bytes, uint64_t send_ts_us) {
    xgw_sent_frame_t *slot;
    if (session == NULL) {
        return;
    }
    if (session->outstanding_count >= XGW_MAX_OUTSTANDING_FRAMES) {
        memmove(session->outstanding,
                session->outstanding + 1,
                (XGW_MAX_OUTSTANDING_FRAMES - 1U) * sizeof(session->outstanding[0]));
        session->outstanding_count = XGW_MAX_OUTSTANDING_FRAMES - 1U;
    }
    slot = &session->outstanding[session->outstanding_count++];
    memset(slot, 0, sizeof(*slot));
    slot->sequence = sequence;
    slot->wire_bytes = wire_bytes;
    slot->send_ts_us = send_ts_us;
    xgw_cc_on_send(&session->cc, wire_bytes, send_ts_us * 1000ULL);
}

/* ---- per-stream 传输状态池(per-stream 多路复用底座) ---- */

void xgw_stream_xport_pool_init(xgw_stream_xport_pool_t *pool) {
    if (pool == NULL) {
        return;
    }
    memset(pool, 0, sizeof(*pool));
}

xgw_stream_xport_t *xgw_stream_xport_get(xgw_stream_xport_pool_t *pool,
                                         uint32_t session_id,
                                         uint32_t stream_id,
                                         int create) {
    size_t i;
    xgw_stream_xport_t *free_slot = NULL;
    if (pool == NULL) {
        return NULL;
    }
    for (i = 0; i < XGW_MAX_XPORT_STREAMS; ++i) {
        xgw_stream_xport_t *s = &pool->streams[i];
        if (s->active) {
            if (s->session_id == session_id && s->stream_id == stream_id) {
                return s;
            }
        } else if (free_slot == NULL) {
            free_slot = s;
        }
    }
    if (!create || free_slot == NULL) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = 1;
    free_slot->session_id = session_id;
    free_slot->stream_id = stream_id;
    free_slot->next_tx_seq = 1U;
    free_slot->next_rx_seq = 1U;
    xgw_replay_window_init(&free_slot->replay_window);
    if (pool->count < XGW_MAX_XPORT_STREAMS) {
        pool->count++;
    }
    return free_slot;
}

void xgw_stream_xport_forget(xgw_stream_xport_pool_t *pool,
                             uint32_t session_id,
                             uint32_t stream_id) {
    xgw_stream_xport_t *s = xgw_stream_xport_get(pool, session_id, stream_id, 0);
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
        if (pool->count > 0U) {
            pool->count--;
        }
    }
}

/* 按业务 stream_id 回收该流在所有 session 上的 xport(事件驱动主回收:流 close 时调用)。
 * 业务 stream_id 由前端分配、每条流唯一,关闭时其所有传输状态都应释放。 */
void xgw_stream_xport_forget_stream(xgw_stream_xport_pool_t *pool, uint32_t stream_id) {
    size_t i;
    if (pool == NULL || stream_id == 0U) {
        return;
    }
    for (i = 0; i < XGW_MAX_XPORT_STREAMS; ++i) {
        xgw_stream_xport_t *s = &pool->streams[i];
        if (s->active && s->stream_id == stream_id) {
            memset(s, 0, sizeof(*s));
            if (pool->count > 0U) {
                pool->count--;
            }
        }
    }
}

void xgw_stream_xport_reap(xgw_stream_xport_pool_t *pool, uint64_t now_us, uint64_t idle_us) {
    size_t i;
    if (pool == NULL || idle_us == 0U) {
        return;
    }
    for (i = 0; i < XGW_MAX_XPORT_STREAMS; ++i) {
        xgw_stream_xport_t *s = &pool->streams[i];
        if (!s->active) {
            continue;
        }
        if (s->last_activity_us != 0U && now_us > s->last_activity_us &&
            now_us - s->last_activity_us > idle_us) {
            memset(s, 0, sizeof(*s));
            if (pool->count > 0U) {
                pool->count--;
            }
        }
    }
}

/* 统计某 session(slot 拥塞域)下当前活跃的 stream 数,用于把 cwnd 在 stream 间均分配额。
 * 近窗口活跃判定:有 inflight 或近期有活动。返回至少 1,避免除零。 */
size_t xgw_stream_xport_active_count(const xgw_stream_xport_pool_t *pool,
                                     uint32_t session_id,
                                     uint64_t now_us,
                                     uint64_t active_window_us) {
    size_t i;
    size_t n = 0U;
    if (pool == NULL) {
        return 1U;
    }
    for (i = 0; i < XGW_MAX_XPORT_STREAMS; ++i) {
        const xgw_stream_xport_t *s = &pool->streams[i];
        if (!s->active || s->session_id != session_id) {
            continue;
        }
        if (s->inflight_bytes > 0U) {
            n++;
        } else if (active_window_us > 0U && s->last_activity_us != 0U &&
                   now_us >= s->last_activity_us &&
                   now_us - s->last_activity_us <= active_window_us) {
            n++;
        }
    }
    return n == 0U ? 1U : n;
}

void xgw_session_note_rx(xgw_session_t *session, uint64_t sequence, size_t wire_bytes, uint64_t now_us) {
    if (session == NULL) {
        return;
    }
    if (session->ack_pending_since_us == 0U) {
        session->ack_pending_since_us = now_us;
    }
    session->pending_ack.largest_acked = max_u64(session->pending_ack.largest_acked, sequence);
    session->pending_ack.packets_acked++;
    session->pending_ack.bytes_acked += (uint32_t) wire_bytes;
    session->pending_ack.ack_delay_us = now_us > session->ack_pending_since_us ? now_us - session->ack_pending_since_us : 0U;
}

int xgw_session_should_flush_feedback(const xgw_session_t *session, uint64_t now_us) {
    return xgw_session_should_flush_feedback_hint(session, now_us, NULL);
}

int xgw_session_should_flush_feedback_hint(const xgw_session_t *session, uint64_t now_us, const xgw_flow_hint_t *hint) {
    uint32_t packet_threshold;
    uint32_t bytes_threshold;
    uint64_t cadence_us;
    if (session == NULL ||
        session->pending_ack.largest_acked == 0U ||
        session->pending_ack.packets_acked == 0U) {
        return 0;
    }
    packet_threshold = session_feedback_packet_threshold(hint);
    bytes_threshold = session_feedback_bytes_threshold(hint);
    cadence_us = session_feedback_cadence_us(hint);
    if (session->pending_ack.packets_acked >= packet_threshold) {
        return 1;
    }
    if (session->pending_ack.bytes_acked >= bytes_threshold) {
        return 1;
    }
    if (session->last_feedback_tx_us == 0U) {
        return 1;
    }
    if (now_us > session->last_feedback_tx_us && now_us - session->last_feedback_tx_us >= cadence_us) {
        return 1;
    }
    if (session->ack_pending_since_us != 0U &&
        now_us > session->ack_pending_since_us &&
        now_us - session->ack_pending_since_us >= cadence_us) {
        return 1;
    }
    return 0;
}

int xgw_session_flush_feedback(xgw_session_t *session, uint64_t now_us, xgw_ack_info_t *ack) {
    return xgw_session_flush_feedback_hint(session, now_us, NULL, ack);
}

int xgw_session_flush_feedback_hint(xgw_session_t *session,
                                    uint64_t now_us,
                                    const xgw_flow_hint_t *hint,
                                    xgw_ack_info_t *ack) {
    if (session == NULL || ack == NULL || !xgw_session_should_flush_feedback_hint(session, now_us, hint)) {
        return 0;
    }
    *ack = session->pending_ack;
    ack->ack_delay_us = session->ack_pending_since_us != 0U && now_us > session->ack_pending_since_us
                        ? now_us - session->ack_pending_since_us
                        : 0U;
    ack->latest_rtt_us = 0U;
    session->last_feedback_tx_us = now_us;
    session->ack_pending_since_us = 0U;
    session->pending_ack.packets_acked = 0U;
    session->pending_ack.bytes_acked = 0U;
    session->pending_ack.packets_lost = 0U;
    session->pending_ack.bytes_lost = 0U;
    session->pending_ack.ack_delay_us = 0U;
    return 1;
}

void xgw_session_mark_acked(xgw_session_t *session, const xgw_ack_info_t *ack, uint64_t now_us) {
    size_t i = 0;
    if (session == NULL || ack == NULL) {
        return;
    }
    session->last_rx_ack = *ack;
    while (i < session->outstanding_count) {
        xgw_sent_frame_t *slot = &session->outstanding[i];
        if (!slot->acked && slot->sequence <= ack->largest_acked) {
            uint64_t rtt_us = ack->latest_rtt_us;
            if (rtt_us == 0U && now_us > slot->send_ts_us) {
                rtt_us = now_us - slot->send_ts_us;
            }
            slot->acked = 1;
            xgw_cc_on_ack(&session->cc, slot->wire_bytes, rtt_us, now_us * 1000ULL);
            memmove(slot, slot + 1, (session->outstanding_count - i - 1U) * sizeof(*slot));
            session->outstanding_count--;
            continue;
        }
        ++i;
    }
    session->latest_peer_timestamp_us = now_us;
}

void xgw_session_mark_loss_before(xgw_session_t *session, uint64_t largest_acked, uint64_t now_us) {
    size_t i = 0;
    if (session == NULL) {
        return;
    }
    while (i < session->outstanding_count) {
        xgw_sent_frame_t *slot = &session->outstanding[i];
        if (slot->sequence + 3U < largest_acked) {
            xgw_cc_on_loss(&session->cc, slot->wire_bytes, now_us * 1000ULL);
            memmove(slot, slot + 1, (session->outstanding_count - i - 1U) * sizeof(*slot));
            session->outstanding_count--;
            continue;
        }
        ++i;
    }
    session->last_rx_ack.largest_acked = max_u64(session->last_rx_ack.largest_acked, largest_acked);
}

void xgw_session_reset_receive_state(xgw_session_t *session) {
    if (session == NULL) {
        return;
    }
    xgw_replay_window_init(&session->replay_window);
    memset(&session->pending_ack, 0, sizeof(session->pending_ack));
    memset(&session->last_rx_ack, 0, sizeof(session->last_rx_ack));
    session->ack_pending_since_us = 0U;
    session->next_rx_seq = 1U;
}

void xgw_session_reset_transport_state(xgw_session_t *session) {
    if (session == NULL) {
        return;
    }
    xgw_session_reset_receive_state(session);
    session->next_tx_seq = 1U;
    session->last_feedback_tx_us = 0U;
    session->last_control_tx_us = 0U;
    session->last_control_rx_us = 0U;
    session->latest_peer_timestamp_us = 0U;
    session->outstanding_count = 0U;
    session->cc.inflight_bytes = 0U;
    session->cc.next_send_time_ns = 0U;
}

void xgw_channel_set_init(xgw_channel_set_t *channels) {
    if (channels == NULL) {
        return;
    }
    memset(channels, 0, sizeof(*channels));
}

static xgw_neighbor_channel_t *channel_slot(xgw_channel_set_t *channels, xgw_channel_direction_t direction) {
    if (channels == NULL) {
        return NULL;
    }
    if (direction == XGW_CHANNEL_PREVIOUS) {
        return &channels->previous;
    }
    if (direction == XGW_CHANNEL_NEXT) {
        return &channels->next;
    }
    return NULL;
}

static int xgw_channel_session_ready_for_peer(const xgw_neighbor_channel_t *channel,
                                              const xgw_session_t *session) {
    if (channel == NULL || session == NULL) {
        return 0;
    }
    if (!session->active) {
        return 0;
    }
    if (session->control_state == XGW_CTRL_ESTABLISHED) {
        return 1;
    }
    if (channel->peer_host[0] == '\0' || channel->peer_port == 0U) {
        return 0;
    }
    if (strcmp(session->remote_addr.host, channel->peer_host) != 0 ||
        session->remote_addr.port != channel->peer_port) {
        return 0;
    }
    return 0;
}

xgw_session_slot_t xgw_flow_class_to_slot(uint8_t flow_class) {
    switch (flow_class) {
        case 1U:
        case 2U:
        case 4U:
        case 5U:
        case 6U:
            return XGW_SESSION_SLOT_CONTROL;
        case 3U:
        case 8U:
            return XGW_SESSION_SLOT_MEDIA;
        default:
            return XGW_SESSION_SLOT_BULK;
    }
}

void xgw_channel_set_pending_slot(xgw_neighbor_channel_t *channel,
                                  xgw_session_slot_t slot,
                                  xgw_session_t *session) {
    if (channel == NULL || slot >= XGW_SESSION_SLOT_COUNT) {
        return;
    }
    channel->pending_slots[slot] = session;
    if (slot == XGW_SESSION_SLOT_BULK || channel->pending_session == NULL) {
        channel->pending_session = session;
    }
}

void xgw_channel_promote_slot(xgw_neighbor_channel_t *channel,
                              xgw_session_slot_t slot) {
    if (channel == NULL || slot >= XGW_SESSION_SLOT_COUNT) {
        return;
    }
    if (xgw_channel_session_ready_for_peer(channel, channel->pending_slots[slot])) {
        channel->active_slots[slot] = channel->pending_slots[slot];
        if (slot == XGW_SESSION_SLOT_BULK || channel->active_session == NULL) {
            channel->active_session = channel->active_slots[slot];
        }
    }
}

xgw_session_t *xgw_channel_slot_session(const xgw_neighbor_channel_t *channel,
                                        xgw_session_slot_t slot,
                                        int require_ready) {
    xgw_session_t *session;
    if (channel == NULL || slot >= XGW_SESSION_SLOT_COUNT) {
        return NULL;
    }
    session = channel->active_slots[slot];
    if (session == NULL && slot == XGW_SESSION_SLOT_BULK) {
        session = channel->active_session;
    }
    if (!require_ready) {
        return session;
    }
    return xgw_channel_session_ready_for_peer(channel, session) ? session : NULL;
}

void xgw_channel_bind(xgw_channel_set_t *channels,
                      xgw_channel_direction_t direction,
                      const char *host,
                      uint16_t port,
                      xgw_session_t *session) {
    xgw_neighbor_channel_t *channel = channel_slot(channels, direction);
    if (channel == NULL) {
        return;
    }
    channel->direction = direction;
    snprintf(channel->peer_host, sizeof(channel->peer_host), "%s", host == NULL ? "" : host);
    channel->peer_port = port;
    memset(channel->pending_slots, 0, sizeof(channel->pending_slots));
    memset(channel->active_slots, 0, sizeof(channel->active_slots));
    channel->pending_slots[XGW_SESSION_SLOT_BULK] = session;
    channel->active_session = NULL;
    channel->pending_session = session;
    channel->state = session == NULL ? XGW_CHANNEL_STATE_INIT : XGW_CHANNEL_STATE_HANDSHAKING;
    channel->generation++;
}

xgw_neighbor_channel_t *xgw_channel_get(xgw_channel_set_t *channels, xgw_channel_direction_t direction) {
    return channel_slot(channels, direction);
}

const xgw_neighbor_channel_t *xgw_channel_get_const(const xgw_channel_set_t *channels, xgw_channel_direction_t direction) {
    return channel_slot((xgw_channel_set_t *) channels, direction);
}

int xgw_channel_ready(const xgw_neighbor_channel_t *channel) {
    return xgw_channel_slot_session(channel, XGW_SESSION_SLOT_BULK, 1) != NULL;
}
