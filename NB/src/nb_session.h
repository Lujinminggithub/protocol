#ifndef NB_SESSION_H
#define NB_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <picoquic.h>

#include "nb_auth.h"
#include "nb_fec.h"
#include "nb_live.h"
#include "nb_lstream.h"
#include "nb_policy.h"
#include "nb_ring.h"
#include "nb_udp.h"
#include "nb_udp_fec.h"
#include "nb_udp_io.h"

typedef struct {
    nb_live_queue_clock_t down_tx;
    nb_live_queue_clock_t up_tx;
    nb_live_queue_clock_t q2t;
} nb_stream_queue_clocks_t;

typedef struct proxy_stream {
    int in_use;
    uint32_t id;                 /* 本地流水号(日志追踪用) */
    /* 上游(left) QUIC: middle/exit 收上一跳的 server stream */
    picoquic_cnx_t* up_cnx;
    uint64_t up_stream_id;
    /* 下游(right) QUIC: entry/middle 到下一跳的 client stream */
    picoquic_cnx_t* down_cnx;
    uint64_t down_stream_id;
    int down_pool_idx;            /* 该下游主 stream 所在连接池槽位, 供附加控制流复用 */
    int down_pool_id;             /* 下一跳连接池注册表索引 */
    int down_opened;             /* 下游 stream 首部已发 */
    /* TCP: entry=客户端 conn; exit=目标 conn; -1=无 */
    int tcp_fd;
    int tcp_connecting;          /* exit: 非阻塞 connect 进行中 */
    /* 首部解析(middle/exit 收 up 首部) */
    int hdr_done;
    char hdr[600];
    size_t hdr_len;
    /* qtx: 面向 QUIC 的待发送缓冲。V1.5 起主数据路径改走 prepare_to_send，避免 add_to_stream
     * 额外排队复制。down_tx=left->right(发下游), up_tx=right->left(写回上游)。 */
    nb_ring_t down_tx;
    nb_stream_queue_clocks_t* queue_clocks;
    int down_tx_fin;
    uint8_t down_prefix[768];
    size_t down_prefix_len;
    size_t down_prefix_off;
    nb_ring_t up_tx;
    int up_tx_fin;
    /* q2t: 面向 TCP 的待写缓冲(entry 写回客户端 / exit 写目标)。动态增长, 永不丢数据;
     * 总量上限由 QUIC connection flow control(max_data) 自然背压 */
    nb_ring_t q2t;
    int q2t_fin;                 /* 待对 TCP 做半关 */
    int need_teardown;           /* 过载保护: q2t 超限, 主循环兜底回收(不在回调栈内拆) */
    int tcp_eof;                 /* 本地 TCP 读到 EOF */
    int tcp_read_paused;
    int upstream_fc_enabled;
    int upstream_fc_blocked;
    int probe_mode;              /* exit internal probe: 1=sink, 2=echo, 4=downlink source */
    uint64_t probe_bytes;
    uint64_t probe_hash;
    uint64_t probe_expected_bytes;
    uint8_t probe_header[NB_PROBE_HEADER_SIZE];
    size_t probe_header_len;
    int probe_ack_sent;
    uint64_t probe_progress_at;
    /* 端到端逻辑流。Middle 仅透传 NBLS 帧；Entry/Exit 保存 replay 与消费 offset。 */
    int logical_mode;
    int logical_endpoint;
    int logical_reconnect_pending;
    int logical_resume_inflight;
    unsigned logical_reconnect_failures;
    uint64_t logical_disconnected_at;
    uint64_t logical_reconnect_last_attempt;
    uint64_t logical_flow_hi;
    uint64_t logical_flow_lo;
    nb_lstream_tx_t logical_tx;
    nb_lstream_decoder_t* logical_decoder;
    uint64_t logical_rx_next;
    uint64_t logical_rx_delivered;
    uint64_t logical_rx_fin_offset;
    int logical_rx_fin_seen;
    int logical_rx_fin_acked_sent;
    uint64_t logical_last_ack_offset;
    uint64_t logical_last_ack_at;
    uint64_t logical_rx_frames[NB_LSTREAM_FIN_ACK + 1];
    /* fin 追踪(去程 left->right, 回程 right->left) */
    int up_fin_seen;             /* 收到 left QUIC fin(仅 middle/exit) */
    int down_fin_seen;           /* 收到 right QUIC fin(仅 entry/middle) */
    char route[300];             /* 本 stream 的 route 首部(日志) */
    uint64_t last_active;        /* 最近一次有数据活动的时刻(us), 空闲超时兜底回收用 */
    int dns_pending;             /* exit: 该流的 target 域名正在异步 DNS 解析中 */
    int target_port;             /* exit: DNS 解析完成后 connect 用的端口 */
    int prio;                    /* 流优先级(例如 ctrl=2 / media=4 / bulk=20), 首部 <prio>; 端到端传递 */
    nb_flow_class_t flow_class;  /* 白名单通过后, TikTok 内部分流分类(ctrl/media/bulk/unknown) */
    nb_flow_lane_t lane_hint;    /* 分流器给出的 lane 建议(latency/bulk) */
    nb_flow_fec_t fec_hint;      /* 分流器给出的 FEC 建议(auto/off/force) */
    char flow_rule[64];          /* 命中的 TikTok 规则名 */
    /* ===== V1.5 纠错型 FEC sidecar 骨架 =====
     * 保持当前 TCP-over-QUIC 主路径不变，在坏跳 middle<->exit 内部额外建立：
     * 1) 一条可靠控制流(FEC START/ACK/NACK/RETX/FIN/RST)
     * 2) 一条连接级 datagram sidecar(先打通 HELLO/ACK 骨架，后续再承载 source/repair symbol) */
    int fec_sidecar_mode;        /* 当前逻辑流是否已切换为 V1.5 sidecar 数据面 */
    int fec_ctrl_only;           /* 该 ps 仅服务于 FEC 控制流(如 exit 收到的控制会话) */
    uint32_t fec_session_id;     /* V1.5 FEC 会话 ID */
    picoquic_cnx_t* fec_ctrl_cnx;/* middle 本地额外打开的 FEC 控制流所在线路 */
    uint64_t fec_ctrl_stream_id;
    int fec_ctrl_opened;
    int fec_ctrl_peer_ready;
    int fec_rx_to_down;          /* sidecar 接收恢复后应继续往下游转发(如 entry->middle 保护段) */
    char fec_route[300];         /* sidecar 会话携带的坏跳目标 route(一般为 T:host:port) */
    char* fec_ctrl_rxbuf;
    size_t fec_ctrl_rxlen;
    picoquic_cnx_t* fec_dg_cnx;  /* sidecar datagram 所属连接(目前与控制流/坏跳数据流同 cnx) */
    uint8_t* fec_dg_tx;
    size_t fec_dg_tx_len;
    size_t fec_dg_tx_cap;
    uint8_t* fec_stage_tx;
    size_t fec_stage_tx_len;
    size_t fec_stage_tx_cap;
    int fec_stage_tx_fin;
    uint64_t fec_dg_sent;
    uint64_t fec_dg_recv;
    uint64_t fec_dg_acked;
    uint64_t fec_dg_lost;
    uint32_t fec_dg_inflight;
    uint32_t fec_dg_inflight_peak;
    nb_fec_config_t fec_config;
    nb_fec_session_t* fec_engine;
    /* SOCKS5 入口(仅 entry -S): 握手阶段 0=greeting 1=request 2=直通 */
    int socks_stage;
    int socks_auth_pending;
    char socks_auth_username[NB_AUTH_NAME_MAX];
    unsigned char socks_auth_fingerprint[NB_AUTH_FINGERPRINT_LEN];
    uint64_t socks_auth_worker_us;
    int tenant_index;
    int tenant_acquired;
    int tenant_udp;
    char tenant_name[64];
    uint64_t tenant_accounted_up;
    uint64_t tenant_accounted_down;
    uint64_t tenant_throttled_until;
    int route_index;
    uint8_t socks_buf[512];
    size_t socks_len;
    uint64_t created_at;
    uint64_t transport_generation;
    uint64_t first_c2s_at;
    uint64_t first_s2c_at;
    uint64_t bytes_c2s;
    uint64_t bytes_s2c;
    uint64_t media_metrics_at;
    uint64_t media_metrics_c2s;
    uint64_t media_metrics_s2c;
    size_t media_down_q_peak;
    size_t media_up_q_peak;
    size_t media_q2t_peak;
    unsigned int high_uplink_windows;
    nb_live_flow_runtime_t live_runtime;
    int downlink_bulk;
    int sched_throttled;
    uint64_t target_connect_at;
    uint64_t target_connect_done_at;
    uint64_t socks_greeting_at;
    uint64_t socks_auth_at;
    uint64_t socks_request_at;
    uint64_t downstream_open_at;
    uint64_t down_prefix_queued_at;
    uint64_t down_prefix_prepare_at;
    uint64_t first_down_queue_at;
    uint64_t first_down_prepare_at;
    uint64_t first_up_queue_at;
    uint64_t first_up_prepare_at;
    uint64_t first_q2t_queue_at;
    uint64_t first_q2t_flush_at;
    int target_connect_state;    /* 0=n/a, 1=pending, 2=ok, 3=failed */
    char peer_addr[96];
    char close_reason[48];
    int udp_mode;
    int udp_association;
    int udp_fd;
    nb_udp_rxq_state_t udp_rxq_state;
    uint32_t udp_session_id;
    uint32_t udp_parent_id;
    uint32_t udp_tx_sequence;
    int udp_close_received;
    char udp_target_host[256];
    int udp_target_port;
    struct sockaddr_storage udp_tcp_peer;
    socklen_t udp_tcp_peer_len;
    struct sockaddr_storage udp_client_peer;
    socklen_t udp_client_peer_len;
    int udp_client_peer_set;
    uint8_t* udp_down_tx;
    size_t udp_down_tx_len;
    size_t udp_down_tx_cap;
    uint8_t* udp_up_tx;
    size_t udp_up_tx_len;
    size_t udp_up_tx_cap;
    uint8_t* udp_pending_tx;
    size_t udp_pending_tx_len;
    size_t udp_pending_tx_cap;
    nb_udp_reassembly_t* udp_reassembly;
    nb_udp_fec_tx_t* udp_fec_tx;
    nb_udp_fec_rx_t* udp_fec_rx;
    uint64_t udp_fec_source_packets;
    uint64_t udp_fec_repairs_sent;
    uint64_t udp_fec_repairs_received;
    uint64_t udp_fec_recovered;
    uint64_t udp_packets_c2s;
    uint64_t udp_packets_s2c;
    uint64_t udp_assoc_raw_rx;
    uint64_t udp_assoc_reject_ip;
    uint64_t udp_assoc_malformed;
    uint64_t udp_assoc_policy_drop;
    uint64_t udp_queue_pressure_dropped;
    uint64_t udp_assoc_first_raw_at;
    uint64_t udp_control_closed_at;
    uint64_t udp_first_local_c2s_at;
    uint64_t udp_first_c2s_rx_at;
    uint64_t udp_first_c2s_prepare_at;
    uint64_t udp_target_ready_at;
    uint64_t udp_first_target_send_at;
    uint64_t udp_first_target_rx_at;
    uint64_t udp_first_s2c_rx_at;
    uint64_t udp_first_s2c_prepare_at;
    uint64_t udp_first_local_s2c_at;
} proxy_stream_t;

#endif
