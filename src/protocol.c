/* 协议层公共逻辑：默认 profile、类型名与头部编解码。 */

#include "xgw_protocol.h"

#include <stdio.h>
#include <string.h>

void xgw_profile_init_default(xgw_profile_t *profile, xgw_profile_mode_t mode) {
    memset(profile, 0, sizeof(*profile));
    profile->mode = mode;
    profile->mtu = 1380;
    profile->payload_size = 1200;
    profile->reorder_window = 128;

    switch (mode) {
        case XGW_PROFILE_BBR:
            profile->fec_data_shards = 4;
            profile->fec_parity_shards = 1;
            profile->pacing_rate_bps = 50000000ULL;
            profile->pacing_interval_us = 200;
            profile->redundant_copies = 1;
            break;
        case XGW_PROFILE_BRUTAL:
            profile->fec_data_shards = 4;
            profile->fec_parity_shards = 2;
            profile->pacing_rate_bps = 200000000ULL;
            profile->pacing_interval_us = 50;
            profile->redundant_copies = 2;
            break;
        case XGW_PROFILE_NONE:
        default:
            profile->fec_data_shards = 0;
            profile->fec_parity_shards = 0;
            profile->pacing_rate_bps = 0;
            profile->pacing_interval_us = 0;
            profile->redundant_copies = 1;
            break;
    }
}

/* 把角色枚举转换成可读字符串。 */
const char *xgw_role_name(xgw_role_t role) {
    switch (role) {
        case XGW_ROLE_MOBILE:
            return "mobile";
        case XGW_ROLE_INGRESS:
            return "ingress";
        case XGW_ROLE_RELAY:
            return "relay";
        case XGW_ROLE_EGRESS:
            return "egress";
        default:
            return "unknown";
    }
}

/* 把 profile 模式转换成可读字符串。 */
const char *xgw_profile_name(xgw_profile_mode_t mode) {
    switch (mode) {
        case XGW_PROFILE_BBR:
            return "live-bbr";
        case XGW_PROFILE_BRUTAL:
            return "live-brutal";
        case XGW_PROFILE_NONE:
        default:
            return "live-none";
    }
}

const char *xgw_congestion_mode_name(xgw_congestion_mode_t mode) {
    switch (mode) {
        case XGW_CC_BBR:
            return "bbr";
        case XGW_CC_BRUTAL:
            return "brutal";
        case XGW_CC_RENO:
            return "reno";
        case XGW_CC_NONE:
        default:
            return "none";
    }
}

const char *xgw_bbr_profile_name(xgw_bbr_profile_t profile) {
    switch (profile) {
        case XGW_BBR_CONSERVATIVE:
            return "conservative";
        case XGW_BBR_AGGRESSIVE:
            return "aggressive";
        case XGW_BBR_STANDARD:
        default:
            return "standard";
    }
}

const char *xgw_message_type_name(uint8_t type) {
    switch (type) {
        case XGW_MESSAGE_HELLO:
            return "hello";
        case XGW_MESSAGE_HELLO_ACK:
            return "hello_ack";
        case XGW_MESSAGE_CONFIRM:
            return "confirm";
        case XGW_MESSAGE_DATA:
            return "data";
        case XGW_MESSAGE_KEEPALIVE:
            return "keepalive";
        case XGW_MESSAGE_FEC:
            return "fec";
        default:
            return "unknown";
    }
}

int xgw_header_encode(const xgw_header_t *header, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (header == NULL || out == NULL || out_len == NULL) {
        return 0;
    }
    if (out_cap < XGW_HEADER_SIZE + payload_len) {
        return 0;
    }
    out[0] = header->version;
    out[1] = header->type;
    out[2] = (uint8_t) (header->flags >> 8U);
    out[3] = (uint8_t) header->flags;
    out[4] = (uint8_t) (header->session_id >> 24U);
    out[5] = (uint8_t) (header->session_id >> 16U);
    out[6] = (uint8_t) (header->session_id >> 8U);
    out[7] = (uint8_t) header->session_id;
    out[8] = (uint8_t) (header->sequence >> 56U);
    out[9] = (uint8_t) (header->sequence >> 48U);
    out[10] = (uint8_t) (header->sequence >> 40U);
    out[11] = (uint8_t) (header->sequence >> 32U);
    out[12] = (uint8_t) (header->sequence >> 24U);
    out[13] = (uint8_t) (header->sequence >> 16U);
    out[14] = (uint8_t) (header->sequence >> 8U);
    out[15] = (uint8_t) header->sequence;
    out[16] = (uint8_t) (header->payload_length >> 8U);
    out[17] = (uint8_t) header->payload_length;
    out[18] = (uint8_t) (header->reserved >> 8U);
    out[19] = (uint8_t) header->reserved;
    if (payload_len > 0U && payload != NULL) {
        memcpy(out + XGW_HEADER_SIZE, payload, payload_len);
    }
    *out_len = XGW_HEADER_SIZE + payload_len;
    return 1;
}

/* 从原始字节流里解析一帧头部。 */
int xgw_header_decode(const uint8_t *packet, size_t packet_len, xgw_header_t *header, const uint8_t **payload, size_t *payload_len) {
    if (packet == NULL || header == NULL || payload == NULL || payload_len == NULL) {
        return 0;
    }
    if (packet_len < XGW_HEADER_SIZE) {
        return 0;
    }
    header->version = packet[0];
    header->type = packet[1];
    header->flags = (uint16_t) (((uint16_t) packet[2] << 8U) | (uint16_t) packet[3]);
    header->session_id = ((uint32_t) packet[4] << 24U) |
                         ((uint32_t) packet[5] << 16U) |
                         ((uint32_t) packet[6] << 8U) |
                         (uint32_t) packet[7];
    header->sequence = ((uint64_t) packet[8] << 56U) |
                       ((uint64_t) packet[9] << 48U) |
                       ((uint64_t) packet[10] << 40U) |
                       ((uint64_t) packet[11] << 32U) |
                       ((uint64_t) packet[12] << 24U) |
                       ((uint64_t) packet[13] << 16U) |
                       ((uint64_t) packet[14] << 8U) |
                       (uint64_t) packet[15];
    header->payload_length = (uint16_t) (((uint16_t) packet[16] << 8U) | (uint16_t) packet[17]);
    header->reserved = (uint16_t) (((uint16_t) packet[18] << 8U) | (uint16_t) packet[19]);
    if (packet_len < ((size_t) XGW_HEADER_SIZE + (size_t) header->payload_length)) {
        return 0;
    }
    *payload = packet + XGW_HEADER_SIZE;
    *payload_len = header->payload_length;
    return 1;
}

int xgw_parse_congestion_mode(const char *value, xgw_congestion_mode_t *mode) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "none") == 0) {
        *mode = XGW_CC_NONE;
        return 1;
    }
    if (strcmp(value, "bbr") == 0) {
        *mode = XGW_CC_BBR;
        return 1;
    }
    if (strcmp(value, "brutal") == 0) {
        *mode = XGW_CC_BRUTAL;
        return 1;
    }
    if (strcmp(value, "reno") == 0) {
        *mode = XGW_CC_RENO;
        return 1;
    }
    return 0;
}

int xgw_parse_bbr_profile(const char *value, xgw_bbr_profile_t *profile) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "standard") == 0) {
        *profile = XGW_BBR_STANDARD;
        return 1;
    }
    if (strcmp(value, "conservative") == 0) {
        *profile = XGW_BBR_CONSERVATIVE;
        return 1;
    }
    if (strcmp(value, "aggressive") == 0) {
        *profile = XGW_BBR_AGGRESSIVE;
        return 1;
    }
    return 0;
}
