#ifndef XGW_SESSION_H
#define XGW_SESSION_H

/* Session 与 replay window 定义。 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define XGW_MAX_SESSIONS 256
#define XGW_REPLAY_BITMAP_WORDS 4

/* 用位图记录已接收序号，防止重放。 */
typedef struct xgw_replay_window {
    uint64_t max_seq;
    uint64_t bitmap[XGW_REPLAY_BITMAP_WORDS];
} xgw_replay_window_t;

/* 远端地址的最小表示。 */
typedef struct xgw_remote_addr {
    char host[64];
    uint16_t port;
} xgw_remote_addr_t;

/* 一个活动 session 的运行时状态。 */
typedef struct xgw_session {
    uint32_t id;
    char group_id[64];
    char tunnel_ip[48];
    xgw_remote_addr_t remote_addr;
    uint64_t next_tx_seq;
    uint64_t next_rx_seq;
    time_t last_seen;
    xgw_replay_window_t replay_window;
    int active;
} xgw_session_t;

/* 固定大小的 session 表。 */
typedef struct xgw_session_table {
    xgw_session_t sessions[XGW_MAX_SESSIONS];
    size_t count;
} xgw_session_table_t;

/* 初始化 replay window。 */
void xgw_replay_window_init(xgw_replay_window_t *window);
/* 检查并接受一个序号，失败表示重放或超窗。 */
int xgw_replay_window_accept(xgw_replay_window_t *window, uint64_t seq, uint32_t max_reorder_window);

/* 初始化 session 表。 */
void xgw_session_table_init(xgw_session_table_t *table);
/* 按 session id 查找 session。 */
xgw_session_t *xgw_session_find_by_id(xgw_session_table_t *table, uint32_t session_id);
/* 按 tunnel IP 查找 session。 */
xgw_session_t *xgw_session_find_by_tunnel_ip(xgw_session_table_t *table, const char *tunnel_ip);
/* 按需创建或更新 session。 */
xgw_session_t *xgw_session_upsert(xgw_session_table_t *table, uint32_t session_id, const char *group_id, const char *tunnel_ip, const char *host, uint16_t port, time_t now);
/* 更新时间戳。 */
void xgw_session_touch(xgw_session_t *session, time_t now);

#endif
