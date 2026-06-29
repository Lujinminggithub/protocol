#ifndef XGW_SESSION_H
#define XGW_SESSION_H

/* Session、replay window、控制面状态与发送采样状态定义。 */

#include "xgw_cc.h"
#include "xgw_coremodel.h"
#include "xgw_protocol.h"
#include "xgw_security.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define XGW_MAX_SESSIONS 256
#define XGW_REPLAY_BITMAP_WORDS 4
#define XGW_MAX_OUTSTANDING_FRAMES 512
/* per-stream 多路复用:全局 stream 传输状态池上限(对齐 XGW_STREAM_SCHED_CAP/bridge target map),
 * 与 session 数解耦。单 xport 的在途窗口用较小值即可(QUIC 单 stream 也不需 512 帧在途)。 */
#define XGW_MAX_XPORT_STREAMS 1024
#define XGW_MAX_STREAM_OUTSTANDING 128

typedef struct xgw_replay_window {
    uint64_t max_seq;
    uint64_t bitmap[XGW_REPLAY_BITMAP_WORDS];
} xgw_replay_window_t;

typedef struct xgw_remote_addr {
    char host[64];
    uint16_t port;
} xgw_remote_addr_t;

typedef struct xgw_sent_frame {
    uint64_t sequence;
    size_t wire_bytes;
    uint64_t send_ts_us;
    int acked;
} xgw_sent_frame_t;

typedef enum xgw_channel_direction {
    XGW_CHANNEL_NONE = 0,
    XGW_CHANNEL_PREVIOUS,
    XGW_CHANNEL_NEXT
} xgw_channel_direction_t;

typedef enum xgw_channel_state {
    XGW_CHANNEL_STATE_INIT = 0,
    XGW_CHANNEL_STATE_HANDSHAKING,
    XGW_CHANNEL_STATE_ESTABLISHED,
    XGW_CHANNEL_STATE_DRAINING,
    XGW_CHANNEL_STATE_CLOSED
} xgw_channel_state_t;

typedef enum xgw_session_slot {
    XGW_SESSION_SLOT_CONTROL = 0,
    XGW_SESSION_SLOT_MEDIA,
    XGW_SESSION_SLOT_BULK,
    XGW_SESSION_SLOT_COUNT
} xgw_session_slot_t;

/*
 * transport segment 鏄 C 鍚庣閭诲眳閾捐矾浼氳瘽鐨勮寖鍥达紝鐢ㄦ潵鏄惧紡鍖哄垎锛?
 * 1. mobile -> ingress
 * 2. ingress -> relay
 * 3. relay -> egress
 *
 * 瀹冧笉绛変簬鍓嶇 front_session_id锛屼篃涓嶇瓑浜庢湰鍦?bridge stream_id銆?
 */
typedef enum xgw_transport_segment {
    XGW_TRANSPORT_SEGMENT_UNKNOWN = 0,
    XGW_TRANSPORT_SEGMENT_MOBILE_INGRESS,
    XGW_TRANSPORT_SEGMENT_INGRESS_RELAY,
    XGW_TRANSPORT_SEGMENT_RELAY_EGRESS
} xgw_transport_segment_t;

typedef struct xgw_transport_scope {
    char line_id[XGW_MAX_NAME_LEN];
    xgw_transport_segment_t segment;
    xgw_session_slot_t slot;
    char peer_host[64];
    uint16_t peer_port;
} xgw_transport_scope_t;

typedef struct xgw_session {
    uint32_t id;
    char group_id[64];
    char tunnel_ip[48];
    xgw_remote_addr_t remote_addr;
    xgw_transport_scope_t scope;
    uint64_t next_tx_seq;
    uint64_t next_rx_seq;
    uint64_t last_keepalive_us;
    uint64_t last_feedback_tx_us;
    uint64_t last_control_tx_us;
    uint64_t last_control_rx_us;
    uint64_t latest_peer_timestamp_us;
    uint64_t ack_pending_since_us;
    time_t last_seen;
    xgw_replay_window_t replay_window;
    xgw_control_state_t control_state;
    xgw_security_material_t security;
    xgw_ack_info_t pending_ack;
    xgw_ack_info_t last_rx_ack;
    xgw_cc_t cc;
    uint32_t last_fec_parity_used;
    xgw_sent_frame_t outstanding[XGW_MAX_OUTSTANDING_FRAMES];
    size_t outstanding_count;
    int active;
} xgw_session_t;

typedef struct xgw_session_table {
    xgw_session_t sessions[XGW_MAX_SESSIONS];
    size_t count;
} xgw_session_table_t;

/*
 * per-stream 传输状态:每条业务流(stream_id)独立的发送窗口/序号/重组/丢包恢复。
 * 挂在 session(= 某 slot 拥塞域)之下,但发送窗口与序号空间 per-stream 独立,
 * 从而消除"同 slot 一条慢 stream 占满窗口拖垮其他 stream"的 head-of-line blocking。
 * 拥塞控制的带宽估计仍由 session->cc(slot 共享 BBR)负责;此处只做 per-stream 窗口记账。
 * 用全局池存储(与 session 数解耦),按 (session_id, stream_id) 查找。
 */
typedef struct xgw_stream_xport {
    int active;
    uint32_t session_id;                 /* 所属 session(slot 拥塞域) */
    uint32_t stream_id;                  /* 业务流标识(贯穿前端→三跳→日志) */
    uint64_t next_tx_seq;                /* per-stream 发送序号空间 */
    uint64_t next_rx_seq;                /* per-stream 接收序号(重组用) */
    xgw_replay_window_t replay_window;   /* per-stream 重放/重排窗口 */
    xgw_sent_frame_t outstanding[XGW_MAX_STREAM_OUTSTANDING]; /* per-stream 在途窗口 */
    size_t outstanding_count;
    uint64_t inflight_bytes;             /* per-stream 在途字节(窗口门控用) */
    xgw_ack_info_t pending_ack;          /* per-stream 待回 ACK */
    uint64_t last_activity_us;
} xgw_stream_xport_t;

typedef struct xgw_stream_xport_pool {
    xgw_stream_xport_t streams[XGW_MAX_XPORT_STREAMS];
    size_t count;
} xgw_stream_xport_pool_t;

void xgw_stream_xport_pool_init(xgw_stream_xport_pool_t *pool);
/* 查找 (session_id, stream_id) 对应的 xport;create=1 时不存在则分配(池满返回 NULL)。 */
xgw_stream_xport_t *xgw_stream_xport_get(xgw_stream_xport_pool_t *pool,
                                         uint32_t session_id,
                                         uint32_t stream_id,
                                         int create);
void xgw_stream_xport_forget(xgw_stream_xport_pool_t *pool,
                             uint32_t session_id,
                             uint32_t stream_id);
/* 按业务 stream_id 回收该流在所有 session 上的 xport(流 close 事件驱动的主回收路径)。 */
void xgw_stream_xport_forget_stream(xgw_stream_xport_pool_t *pool, uint32_t stream_id);
/* 回收空闲超时的 xport(now_us - last_activity_us 超阈值)。 */
void xgw_stream_xport_reap(xgw_stream_xport_pool_t *pool, uint64_t now_us, uint64_t idle_us);
/* 统计某 session 下活跃 stream 数(有 inflight 或近期活动),用于 cwnd 配额均分。返回≥1。 */
size_t xgw_stream_xport_active_count(const xgw_stream_xport_pool_t *pool,
                                     uint32_t session_id,
                                     uint64_t now_us,
                                     uint64_t active_window_us);

typedef struct xgw_neighbor_channel {
    xgw_channel_direction_t direction;
    char peer_host[64];
    uint16_t peer_port;
    xgw_session_t *pending_slots[XGW_SESSION_SLOT_COUNT];
    xgw_session_t *active_slots[XGW_SESSION_SLOT_COUNT];
    xgw_session_t *pending_session;
    xgw_session_t *active_session;
    uint64_t generation;
    xgw_channel_state_t state;
} xgw_neighbor_channel_t;

typedef struct xgw_channel_set {
    xgw_neighbor_channel_t previous;
    xgw_neighbor_channel_t next;
} xgw_channel_set_t;

void xgw_replay_window_init(xgw_replay_window_t *window);
int xgw_replay_window_accept(xgw_replay_window_t *window, uint64_t seq, uint32_t max_reorder_window);

void xgw_session_table_init(xgw_session_table_t *table);
xgw_session_t *xgw_session_find_by_id(xgw_session_table_t *table, uint32_t session_id);
xgw_session_t *xgw_session_find_by_peer(xgw_session_table_t *table,
                                        uint32_t session_id,
                                        const char *host,
                                        uint16_t port);
xgw_session_t *xgw_session_find_by_tunnel_ip(xgw_session_table_t *table, const char *tunnel_ip);
xgw_session_t *xgw_session_upsert(xgw_session_table_t *table,
                                  uint32_t session_id,
                                  const char *group_id,
                                  const char *tunnel_ip,
                                  const char *host,
                                  uint16_t port,
                                  time_t now);
void xgw_session_touch(xgw_session_t *session, time_t now);
void xgw_session_record_send(xgw_session_t *session, uint64_t sequence, size_t wire_bytes, uint64_t send_ts_us);
void xgw_session_note_rx(xgw_session_t *session, uint64_t sequence, size_t wire_bytes, uint64_t now_us);
int xgw_session_should_flush_feedback(const xgw_session_t *session, uint64_t now_us);
int xgw_session_should_flush_feedback_hint(const xgw_session_t *session, uint64_t now_us, const xgw_flow_hint_t *hint);
int xgw_session_flush_feedback(xgw_session_t *session, uint64_t now_us, xgw_ack_info_t *ack);
int xgw_session_flush_feedback_hint(xgw_session_t *session, uint64_t now_us, const xgw_flow_hint_t *hint, xgw_ack_info_t *ack);
void xgw_session_mark_acked(xgw_session_t *session, const xgw_ack_info_t *ack, uint64_t now_us);
void xgw_session_mark_loss_before(xgw_session_t *session, uint64_t largest_acked, uint64_t now_us);
void xgw_session_reset_receive_state(xgw_session_t *session);
void xgw_session_reset_transport_state(xgw_session_t *session);
const char *xgw_transport_segment_name(xgw_transport_segment_t segment);
void xgw_transport_scope_set(xgw_transport_scope_t *scope,
                             const char *line_id,
                             xgw_transport_segment_t segment,
                             xgw_session_slot_t slot,
                             const char *peer_host,
                             uint16_t peer_port);
const char *xgw_transport_scope_summary(const xgw_transport_scope_t *scope,
                                        char *buf,
                                        size_t buf_len);
void xgw_session_set_transport_scope(xgw_session_t *session,
                                     const char *line_id,
                                     xgw_transport_segment_t segment,
                                     xgw_session_slot_t slot,
                                     const char *peer_host,
                                     uint16_t peer_port);
void xgw_channel_set_init(xgw_channel_set_t *channels);
void xgw_channel_bind(xgw_channel_set_t *channels,
                      xgw_channel_direction_t direction,
                      const char *host,
                      uint16_t port,
                      xgw_session_t *session);
xgw_session_slot_t xgw_flow_class_to_slot(uint8_t flow_class);
xgw_session_t *xgw_channel_slot_session(const xgw_neighbor_channel_t *channel,
                                        xgw_session_slot_t slot,
                                        int require_ready);
void xgw_channel_set_pending_slot(xgw_neighbor_channel_t *channel,
                                  xgw_session_slot_t slot,
                                  xgw_session_t *session);
void xgw_channel_promote_slot(xgw_neighbor_channel_t *channel,
                              xgw_session_slot_t slot);
xgw_neighbor_channel_t *xgw_channel_get(xgw_channel_set_t *channels, xgw_channel_direction_t direction);
const xgw_neighbor_channel_t *xgw_channel_get_const(const xgw_channel_set_t *channels, xgw_channel_direction_t direction);
int xgw_channel_ready(const xgw_neighbor_channel_t *channel);

#endif
