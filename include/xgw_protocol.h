#ifndef XGW_PROTOCOL_H
#define XGW_PROTOCOL_H

/* 协议基础定义：角色、profile、公共帧头、控制面负载与编解码接口。 */

#include <stddef.h>
#include <stdint.h>

#define XGW_MAX_ENDPOINTS 8
#define XGW_MAX_LINES 16
#define XGW_MAX_NAME_LEN 64
#define XGW_HEADER_SIZE 24
#define XGW_PROTOCOL_VERSION 2U
#define XGW_FRAGMENT_HEADER_SIZE 10
#define XGW_FEC_HEADER_SIZE 12
#define XGW_NEGOTIATION_PATH "/auth"
#define XGW_NEGOTIATION_HOST "xgw"
#define XGW_FLAG_SECURE 0x0001U
#define XGW_FLAG_ACK_ELICITING 0x0002U
#define XGW_FLAG_CONTROL 0x0004U

typedef enum xgw_message_type {
    XGW_MESSAGE_HELLO = 1,
    XGW_MESSAGE_HELLO_ACK = 2,
    XGW_MESSAGE_CONFIRM = 3,
    XGW_MESSAGE_DATA = 4,
    XGW_MESSAGE_KEEPALIVE = 5,
    XGW_MESSAGE_FEC = 6,
    XGW_MESSAGE_ACK = 7
} xgw_message_type_t;

typedef enum xgw_congestion_mode {
    XGW_CC_BBR = 0,
    XGW_CC_BRUTAL
} xgw_congestion_mode_t;

typedef enum xgw_bbr_profile {
    XGW_BBR_STANDARD = 0
} xgw_bbr_profile_t;

typedef enum xgw_role {
    XGW_ROLE_UNKNOWN = 0,
    XGW_ROLE_MOBILE,
    XGW_ROLE_INGRESS,
    XGW_ROLE_RELAY,
    XGW_ROLE_EGRESS
} xgw_role_t;

typedef enum xgw_profile_mode {
    XGW_PROFILE_BBR = 0,
    XGW_PROFILE_BRUTAL
} xgw_profile_mode_t;

typedef enum xgw_control_state {
    XGW_CTRL_INIT = 0,
    XGW_CTRL_HELLO_SENT,
    XGW_CTRL_HELLO_RCVD,
    XGW_CTRL_CONFIRM_SENT,
    XGW_CTRL_ESTABLISHED
} xgw_control_state_t;

/* 固定链路中的一个节点定义。 */
typedef struct xgw_endpoint {
    char name[XGW_MAX_NAME_LEN];
    char address[XGW_MAX_NAME_LEN];
    char public_address[XGW_MAX_NAME_LEN];
    char private_address[XGW_MAX_NAME_LEN];
    xgw_role_t role;
    int stable;
} xgw_endpoint_t;

/* 一组链路 profile 的核心传输参数。 */
typedef struct xgw_profile {
    xgw_profile_mode_t mode;
    uint32_t mtu;
    uint32_t payload_size;
    uint32_t reorder_window;
    uint32_t fec_data_shards;
    uint32_t fec_parity_shards;
    uint64_t pacing_rate_bps;
    uint32_t pacing_interval_us;
    uint32_t redundant_copies;
} xgw_profile_t;

/* 协议协商结果，参考 HY2 的带宽与能力协商思路。 */
typedef struct xgw_negotiation_info {
    int authenticated;
    int udp_enabled;
    uint64_t local_rx_bps;
    uint64_t peer_rx_bps;
    int peer_rx_auto;
    xgw_congestion_mode_t congestion_mode;
    xgw_bbr_profile_t bbr_profile;
    char auth_token[128];
} xgw_negotiation_info_t;

/* ACK 与链路反馈摘要，用于驱动 RTT/loss/bandwidth 闭环。 */
typedef struct xgw_ack_info {
    uint64_t largest_acked;
    uint64_t ack_delay_us;
    uint64_t latest_rtt_us;
    uint32_t packets_acked;
    uint32_t packets_lost;
    uint32_t bytes_acked;
    uint32_t bytes_lost;
} xgw_ack_info_t;

/* HELLO 携带首轮带宽、MTU、特性与认证摘要。 */
typedef struct xgw_hello_payload {
    uint32_t features;
    uint32_t mtu_offer;
    uint64_t rx_bps;
    uint64_t tx_bps;
    uint64_t nonce;
    uint8_t auth_tag[16];
} xgw_hello_payload_t;

/* HELLO_ACK 反馈协商结果与对端 nonce。 */
typedef struct xgw_hello_ack_payload {
    uint32_t features;
    uint32_t mtu_accept;
    uint64_t rx_bps;
    uint64_t tx_bps;
    uint64_t hello_nonce;
    uint64_t ack_nonce;
    uint8_t auth_tag[16];
} xgw_hello_ack_payload_t;

/* CONFIRM 对双向 nonce 做最终确认。 */
typedef struct xgw_confirm_payload {
    uint64_t hello_nonce;
    uint64_t ack_nonce;
    uint8_t auth_tag[16];
} xgw_confirm_payload_t;

/* KEEPALIVE 既保活也顺带回传 ACK 信息。 */
typedef struct xgw_keepalive_payload {
    uint64_t timestamp_us;
    xgw_ack_info_t ack;
} xgw_keepalive_payload_t;

/* 固定路径拓扑，按 hop 顺序排列。 */
typedef struct xgw_fixed_path {
    xgw_endpoint_t hops[XGW_MAX_ENDPOINTS];
    size_t hop_count;
} xgw_fixed_path_t;

/* 所有数据帧共享的头部格式。
 * v2:新增 stream_id,把 per-stream 多路复用下沉到隧道层。sequence 含义从 per-session
 * 改为 per-stream(每条业务流独立序号空间),stream_id 作对端分发/重组/ACK 的键。 */
typedef struct xgw_header {
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t session_id;
    uint32_t stream_id;
    uint64_t sequence;
    uint16_t payload_length;
    uint16_t reserved;
} xgw_header_t;

void xgw_profile_init_default(xgw_profile_t *profile, xgw_profile_mode_t mode);
const char *xgw_role_name(xgw_role_t role);
const char *xgw_profile_name(xgw_profile_mode_t mode);
const char *xgw_congestion_mode_name(xgw_congestion_mode_t mode);
const char *xgw_bbr_profile_name(xgw_bbr_profile_t profile);
const char *xgw_message_type_name(uint8_t type);
int xgw_header_encode(const xgw_header_t *header,
                      const uint8_t *payload,
                      size_t payload_len,
                      uint8_t *out,
                      size_t out_cap,
                      size_t *out_len);
int xgw_header_decode(const uint8_t *packet,
                      size_t packet_len,
                      xgw_header_t *header,
                      const uint8_t **payload,
                      size_t *payload_len);
int xgw_parse_congestion_mode(const char *value, xgw_congestion_mode_t *mode);
int xgw_parse_bbr_profile(const char *value, xgw_bbr_profile_t *profile);
int xgw_endpoint_pick_address(const xgw_endpoint_t *endpoint,
                              xgw_role_t local_role,
                              xgw_role_t peer_role,
                              char *out,
                              size_t out_len);
size_t xgw_encode_hello_payload(const xgw_hello_payload_t *hello, uint8_t *out, size_t out_cap);
int xgw_decode_hello_payload(const uint8_t *payload, size_t payload_len, xgw_hello_payload_t *hello);
size_t xgw_encode_hello_ack_payload(const xgw_hello_ack_payload_t *hello_ack, uint8_t *out, size_t out_cap);
int xgw_decode_hello_ack_payload(const uint8_t *payload, size_t payload_len, xgw_hello_ack_payload_t *hello_ack);
size_t xgw_encode_confirm_payload(const xgw_confirm_payload_t *confirm, uint8_t *out, size_t out_cap);
int xgw_decode_confirm_payload(const uint8_t *payload, size_t payload_len, xgw_confirm_payload_t *confirm);
size_t xgw_encode_keepalive_payload(const xgw_keepalive_payload_t *keepalive, uint8_t *out, size_t out_cap);
int xgw_decode_keepalive_payload(const uint8_t *payload, size_t payload_len, xgw_keepalive_payload_t *keepalive);

#endif
