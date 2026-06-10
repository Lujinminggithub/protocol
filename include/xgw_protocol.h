#ifndef XGW_PROTOCOL_H
#define XGW_PROTOCOL_H

/* 协议基础定义：角色、profile、公共帧头与基础编解码接口。 */

#include <stddef.h>
#include <stdint.h>

#define XGW_MAX_ENDPOINTS 8
#define XGW_MAX_NAME_LEN 64
#define XGW_HEADER_SIZE 20
#define XGW_FRAGMENT_HEADER_SIZE 8
#define XGW_FEC_HEADER_SIZE 6
#define XGW_NEGOTIATION_PATH "/auth"
#define XGW_NEGOTIATION_HOST "xgw"

typedef enum xgw_message_type {
    XGW_MESSAGE_HELLO = 1,
    XGW_MESSAGE_HELLO_ACK = 2,
    XGW_MESSAGE_CONFIRM = 3,
    XGW_MESSAGE_DATA = 4,
    XGW_MESSAGE_KEEPALIVE = 5,
    XGW_MESSAGE_FEC = 6
} xgw_message_type_t;

typedef enum xgw_congestion_mode {
    XGW_CC_NONE = 0,
    XGW_CC_BBR,
    XGW_CC_BRUTAL,
    XGW_CC_RENO
} xgw_congestion_mode_t;

typedef enum xgw_bbr_profile {
    XGW_BBR_STANDARD = 0,
    XGW_BBR_CONSERVATIVE,
    XGW_BBR_AGGRESSIVE
} xgw_bbr_profile_t;

typedef enum xgw_role {
    XGW_ROLE_UNKNOWN = 0,
    XGW_ROLE_MOBILE,
    XGW_ROLE_INGRESS,
    XGW_ROLE_RELAY,
    XGW_ROLE_EGRESS
} xgw_role_t;

typedef enum xgw_profile_mode {
    XGW_PROFILE_NONE = 0,
    XGW_PROFILE_BBR,
    XGW_PROFILE_BRUTAL
} xgw_profile_mode_t;

/* 固定链路中的一个节点定义。 */
typedef struct xgw_endpoint {
    char name[XGW_MAX_NAME_LEN];
    char address[XGW_MAX_NAME_LEN];
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

/* 协议协商结果，参考 HY2 的带宽与功能协商思路。 */
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

/* 固定路径拓扑，按 hop 顺序排列。 */
typedef struct xgw_fixed_path {
    xgw_endpoint_t hops[XGW_MAX_ENDPOINTS];
    size_t hop_count;
} xgw_fixed_path_t;

/* 所有数据帧共享的头部格式。 */
typedef struct xgw_header {
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t session_id;
    uint64_t sequence;
    uint16_t payload_length;
    uint16_t reserved;
} xgw_header_t;

/* 根据模式填充默认 profile 参数。 */
void xgw_profile_init_default(xgw_profile_t *profile, xgw_profile_mode_t mode);
/* 返回角色名字符串。 */
const char *xgw_role_name(xgw_role_t role);
/* 返回 profile 名字符串。 */
const char *xgw_profile_name(xgw_profile_mode_t mode);
/* 返回拥塞控制模式名。 */
const char *xgw_congestion_mode_name(xgw_congestion_mode_t mode);
/* 返回 BBR profile 名。 */
const char *xgw_bbr_profile_name(xgw_bbr_profile_t profile);
/* 返回消息类型名字符串。 */
const char *xgw_message_type_name(uint8_t type);
/* 将头和 payload 编码为一帧。 */
int xgw_header_encode(const xgw_header_t *header, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap, size_t *out_len);
/* 从原始包中解析头和 payload。 */
int xgw_header_decode(const uint8_t *packet, size_t packet_len, xgw_header_t *header, const uint8_t **payload, size_t *payload_len);
/* 规范化拥塞控制名称。 */
int xgw_parse_congestion_mode(const char *value, xgw_congestion_mode_t *mode);
/* 规范化 BBR profile 名称。 */
int xgw_parse_bbr_profile(const char *value, xgw_bbr_profile_t *profile);

#endif
