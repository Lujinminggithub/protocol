/* Session 与 replay window 管理实现。 */

#include "xgw_session.h"

#include <stdio.h>
#include <string.h>

static void replay_mark(xgw_replay_window_t *window, uint64_t delta) {
    size_t word = (size_t) (delta / 64U);
    uint32_t bit = (uint32_t) (delta % 64U);
    if (word >= XGW_REPLAY_BITMAP_WORDS) {
        return;
    }
    window->bitmap[word] |= (1ULL << bit);
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
    if (words > 0) {
        for (i = XGW_REPLAY_BITMAP_WORDS; i > 0; --i) {
            size_t idx = i - 1U;
            if (idx >= words) {
                window->bitmap[idx] = window->bitmap[idx - words];
            } else {
                window->bitmap[idx] = 0;
            }
        }
    }
    if (bits == 0) {
        return;
    }
    for (i = XGW_REPLAY_BITMAP_WORDS; i > 0; --i) {
        size_t idx = i - 1U;
        uint64_t carry = 0;
        if (idx > 0) {
            carry = window->bitmap[idx - 1U] >> (64U - bits);
        }
        window->bitmap[idx] = (window->bitmap[idx] << bits) | carry;
    }
}

void xgw_replay_window_init(xgw_replay_window_t *window) {
    memset(window, 0, sizeof(*window));
}

/* 判断一个序号是否可接受，拒绝重放与超出窗口的数据。 */
int xgw_replay_window_accept(xgw_replay_window_t *window, uint64_t seq, uint32_t max_reorder_window) {
    uint64_t delta;
    if (seq == 0) {
        return 0;
    }
    if (max_reorder_window == 0) {
        max_reorder_window = XGW_REPLAY_BITMAP_WORDS * 64U;
    }
    if (window->max_seq == 0) {
        window->max_seq = seq;
        replay_mark(window, 0);
        return 1;
    }
    if (seq > window->max_seq) {
        replay_shift_left(window, seq - window->max_seq);
        window->max_seq = seq;
        replay_mark(window, 0);
        return 1;
    }
    delta = window->max_seq - seq;
    if (delta >= max_reorder_window) {
        return 0;
    }
    if (replay_is_marked(window, delta)) {
        return 0;
    }
    replay_mark(window, delta);
    return 1;
}

void xgw_session_table_init(xgw_session_table_t *table) {
    memset(table, 0, sizeof(*table));
}

/* 按 session id 查找。 */
xgw_session_t *xgw_session_find_by_id(xgw_session_table_t *table, uint32_t session_id) {
    size_t i;
    for (i = 0; i < table->count; ++i) {
        if (table->sessions[i].active && table->sessions[i].id == session_id) {
            return &table->sessions[i];
        }
    }
    return NULL;
}

/* 按 tunnel IP 查找。 */
xgw_session_t *xgw_session_find_by_tunnel_ip(xgw_session_table_t *table, const char *tunnel_ip) {
    size_t i;
    for (i = 0; i < table->count; ++i) {
        if (table->sessions[i].active && strcmp(table->sessions[i].tunnel_ip, tunnel_ip) == 0) {
            return &table->sessions[i];
        }
    }
    return NULL;
}

/* 创建或更新一个 session。 */
xgw_session_t *xgw_session_upsert(xgw_session_table_t *table, uint32_t session_id, const char *group_id, const char *tunnel_ip, const char *host, uint16_t port, time_t now) {
    xgw_session_t *session = xgw_session_find_by_id(table, session_id);
    if (session == NULL) {
        if (table->count >= XGW_MAX_SESSIONS) {
            return NULL;
        }
        session = &table->sessions[table->count++];
        memset(session, 0, sizeof(*session));
        session->id = session_id;
        session->active = 1;
        session->next_tx_seq = 1;
        session->next_rx_seq = 1;
        xgw_replay_window_init(&session->replay_window);
    }
    snprintf(session->group_id, sizeof(session->group_id), "%s", group_id == NULL ? "" : group_id);
    snprintf(session->tunnel_ip, sizeof(session->tunnel_ip), "%s", tunnel_ip == NULL ? "" : tunnel_ip);
    snprintf(session->remote_addr.host, sizeof(session->remote_addr.host), "%s", host == NULL ? "" : host);
    session->remote_addr.port = port;
    session->last_seen = now;
    return session;
}

/* 更新 session 最近活跃时间。 */
void xgw_session_touch(xgw_session_t *session, time_t now) {
    if (session != NULL) {
        session->last_seen = now;
    }
}
