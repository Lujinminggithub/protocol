/* 协议层公共逻辑：默认 profile、类型名、头部与控制负载编解码。 */

#include "xgw_protocol.h"

#include <stdio.h>
#include <string.h>

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t) (((uint16_t) p[0] << 8U) | (uint16_t) p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24U) |
           ((uint32_t) p[1] << 16U) |
           ((uint32_t) p[2] << 8U) |
           (uint32_t) p[3];
}

static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t) p[0] << 56U) |
           ((uint64_t) p[1] << 48U) |
           ((uint64_t) p[2] << 40U) |
           ((uint64_t) p[3] << 32U) |
           ((uint64_t) p[4] << 24U) |
           ((uint64_t) p[5] << 16U) |
           ((uint64_t) p[6] << 8U) |
           (uint64_t) p[7];
}

static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8U);
    p[1] = (uint8_t) v;
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24U);
    p[1] = (uint8_t) (v >> 16U);
    p[2] = (uint8_t) (v >> 8U);
    p[3] = (uint8_t) v;
}

static void write_be64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t) (v >> 56U);
    p[1] = (uint8_t) (v >> 48U);
    p[2] = (uint8_t) (v >> 40U);
    p[3] = (uint8_t) (v >> 32U);
    p[4] = (uint8_t) (v >> 24U);
    p[5] = (uint8_t) (v >> 16U);
    p[6] = (uint8_t) (v >> 8U);
    p[7] = (uint8_t) v;
}

void xgw_profile_init_default(xgw_profile_t *profile, xgw_profile_mode_t mode) {
    memset(profile, 0, sizeof(*profile));
    profile->mode = mode;
    profile->mtu = 1280U;
    profile->payload_size = 1100U;
    profile->reorder_window = 128U;
    switch (mode) {
        case XGW_PROFILE_BRUTAL:
            profile->fec_data_shards = 6U;
            profile->fec_parity_shards = 3U;
            profile->pacing_rate_bps = 200000000ULL;
            profile->pacing_interval_us = 50U;
            profile->redundant_copies = 2U;
            break;
        case XGW_PROFILE_BBR:
        default:
            profile->fec_data_shards = 4U;
            profile->fec_parity_shards = 2U;
            profile->pacing_rate_bps = 50000000ULL;
            profile->pacing_interval_us = 200U;
            profile->redundant_copies = 1U;
            break;
    }
}

const char *xgw_role_name(xgw_role_t role) {
    switch (role) {
        case XGW_ROLE_MOBILE: return "mobile";
        case XGW_ROLE_INGRESS: return "ingress";
        case XGW_ROLE_RELAY: return "relay";
        case XGW_ROLE_EGRESS: return "egress";
        default: return "unknown";
    }
}

const char *xgw_profile_name(xgw_profile_mode_t mode) {
    switch (mode) {
        case XGW_PROFILE_BRUTAL: return "live-brutal";
        case XGW_PROFILE_BBR:
        default: return "live-bbr";
    }
}

const char *xgw_congestion_mode_name(xgw_congestion_mode_t mode) {
    switch (mode) {
        case XGW_CC_BRUTAL: return "brutal";
        case XGW_CC_BBR:
        default: return "bbr";
    }
}

const char *xgw_bbr_profile_name(xgw_bbr_profile_t profile) {
    (void) profile;
    return "standard";
}

const char *xgw_message_type_name(uint8_t type) {
    switch (type) {
        case XGW_MESSAGE_HELLO: return "hello";
        case XGW_MESSAGE_HELLO_ACK: return "hello_ack";
        case XGW_MESSAGE_CONFIRM: return "confirm";
        case XGW_MESSAGE_DATA: return "data";
        case XGW_MESSAGE_KEEPALIVE: return "keepalive";
        case XGW_MESSAGE_FEC: return "fec";
        case XGW_MESSAGE_ACK: return "ack";
        default: return "unknown";
    }
}

int xgw_header_encode(const xgw_header_t *header,
                      const uint8_t *payload,
                      size_t payload_len,
                      uint8_t *out,
                      size_t out_cap,
                      size_t *out_len) {
    if (header == NULL || out == NULL || out_len == NULL) {
        return 0;
    }
    if (out_cap < XGW_HEADER_SIZE + payload_len) {
        return 0;
    }
    out[0] = header->version;
    out[1] = header->type;
    write_be16(out + 2U, header->flags);
    write_be32(out + 4U, header->session_id);
    write_be32(out + 8U, header->stream_id);
    write_be64(out + 12U, header->sequence);
    write_be16(out + 20U, header->payload_length);
    write_be16(out + 22U, header->reserved);
    if (payload_len > 0U && payload != NULL) {
        memcpy(out + XGW_HEADER_SIZE, payload, payload_len);
    }
    *out_len = XGW_HEADER_SIZE + payload_len;
    return 1;
}

int xgw_header_decode(const uint8_t *packet,
                      size_t packet_len,
                      xgw_header_t *header,
                      const uint8_t **payload,
                      size_t *payload_len) {
    if (packet == NULL || header == NULL || payload == NULL || payload_len == NULL) {
        return 0;
    }
    if (packet_len < XGW_HEADER_SIZE) {
        return 0;
    }
    header->version = packet[0];
    header->type = packet[1];
    header->flags = read_be16(packet + 2U);
    header->session_id = read_be32(packet + 4U);
    header->stream_id = read_be32(packet + 8U);
    header->sequence = read_be64(packet + 12U);
    header->payload_length = read_be16(packet + 20U);
    header->reserved = read_be16(packet + 22U);
    if (packet_len < (size_t) XGW_HEADER_SIZE + header->payload_length) {
        return 0;
    }
    *payload = packet + XGW_HEADER_SIZE;
    *payload_len = header->payload_length;
    return 1;
}

int xgw_parse_congestion_mode(const char *value, xgw_congestion_mode_t *mode) {
    /* 仅保留 bbr / brutal 两种拥塞模式；其余（含历史 none/reno）回退到 bbr 并告警。 */
    if (value == NULL || value[0] == '\0' || strcmp(value, "bbr") == 0) {
        *mode = XGW_CC_BBR;
        return 1;
    }
    if (strcmp(value, "brutal") == 0) {
        *mode = XGW_CC_BRUTAL;
        return 1;
    }
    fprintf(stderr, "xgw: unknown congestion mode '%s', falling back to bbr\n", value);
    *mode = XGW_CC_BBR;
    return 1;
}

int xgw_parse_bbr_profile(const char *value, xgw_bbr_profile_t *profile) {
    /* 仅保留 standard 一档；历史 conservative/aggressive 回退到 standard 并告警。 */
    if (value == NULL || value[0] == '\0' || strcmp(value, "standard") == 0) {
        *profile = XGW_BBR_STANDARD;
        return 1;
    }
    fprintf(stderr, "xgw: unknown bbr_profile '%s', falling back to standard\n", value);
    *profile = XGW_BBR_STANDARD;
    return 1;
}

int xgw_endpoint_pick_address(const xgw_endpoint_t *endpoint,
                              xgw_role_t local_role,
                              xgw_role_t peer_role,
                              char *out,
                              size_t out_len) {
    const char *selected = "";
    int prefer_private = 0;
    if (endpoint == NULL || out == NULL || out_len == 0U) {
        return 0;
    }
    prefer_private =
        (local_role == XGW_ROLE_INGRESS && peer_role == XGW_ROLE_RELAY) ||
        (local_role == XGW_ROLE_RELAY && peer_role == XGW_ROLE_INGRESS);
    if (prefer_private && endpoint->private_address[0] != '\0') {
        selected = endpoint->private_address;
    } else if (endpoint->public_address[0] != '\0') {
        selected = endpoint->public_address;
    } else if (endpoint->private_address[0] != '\0') {
        selected = endpoint->private_address;
    } else {
        selected = endpoint->address;
    }
    if (selected == NULL || selected[0] == '\0') {
        out[0] = '\0';
        return 0;
    }
    snprintf(out, out_len, "%s", selected);
    return 1;
}

size_t xgw_encode_hello_payload(const xgw_hello_payload_t *hello, uint8_t *out, size_t out_cap) {
    if (hello == NULL || out == NULL || out_cap < 40U) {
        return 0U;
    }
    write_be32(out, hello->features);
    write_be32(out + 4U, hello->mtu_offer);
    write_be64(out + 8U, hello->rx_bps);
    write_be64(out + 16U, hello->tx_bps);
    write_be64(out + 24U, hello->nonce);
    memcpy(out + 32U, hello->auth_tag, 16U);
    return 48U;
}

int xgw_decode_hello_payload(const uint8_t *payload, size_t payload_len, xgw_hello_payload_t *hello) {
    if (payload == NULL || hello == NULL || payload_len < 48U) {
        return 0;
    }
    memset(hello, 0, sizeof(*hello));
    hello->features = read_be32(payload);
    hello->mtu_offer = read_be32(payload + 4U);
    hello->rx_bps = read_be64(payload + 8U);
    hello->tx_bps = read_be64(payload + 16U);
    hello->nonce = read_be64(payload + 24U);
    memcpy(hello->auth_tag, payload + 32U, 16U);
    return 1;
}

size_t xgw_encode_hello_ack_payload(const xgw_hello_ack_payload_t *hello_ack, uint8_t *out, size_t out_cap) {
    if (hello_ack == NULL || out == NULL || out_cap < 56U) {
        return 0U;
    }
    write_be32(out, hello_ack->features);
    write_be32(out + 4U, hello_ack->mtu_accept);
    write_be64(out + 8U, hello_ack->rx_bps);
    write_be64(out + 16U, hello_ack->tx_bps);
    write_be64(out + 24U, hello_ack->hello_nonce);
    write_be64(out + 32U, hello_ack->ack_nonce);
    memcpy(out + 40U, hello_ack->auth_tag, 16U);
    return 56U;
}

int xgw_decode_hello_ack_payload(const uint8_t *payload, size_t payload_len, xgw_hello_ack_payload_t *hello_ack) {
    if (payload == NULL || hello_ack == NULL || payload_len < 56U) {
        return 0;
    }
    memset(hello_ack, 0, sizeof(*hello_ack));
    hello_ack->features = read_be32(payload);
    hello_ack->mtu_accept = read_be32(payload + 4U);
    hello_ack->rx_bps = read_be64(payload + 8U);
    hello_ack->tx_bps = read_be64(payload + 16U);
    hello_ack->hello_nonce = read_be64(payload + 24U);
    hello_ack->ack_nonce = read_be64(payload + 32U);
    memcpy(hello_ack->auth_tag, payload + 40U, 16U);
    return 1;
}

size_t xgw_encode_confirm_payload(const xgw_confirm_payload_t *confirm, uint8_t *out, size_t out_cap) {
    if (confirm == NULL || out == NULL || out_cap < 32U) {
        return 0U;
    }
    write_be64(out, confirm->hello_nonce);
    write_be64(out + 8U, confirm->ack_nonce);
    memcpy(out + 16U, confirm->auth_tag, 16U);
    return 32U;
}

int xgw_decode_confirm_payload(const uint8_t *payload, size_t payload_len, xgw_confirm_payload_t *confirm) {
    if (payload == NULL || confirm == NULL || payload_len < 32U) {
        return 0;
    }
    memset(confirm, 0, sizeof(*confirm));
    confirm->hello_nonce = read_be64(payload);
    confirm->ack_nonce = read_be64(payload + 8U);
    memcpy(confirm->auth_tag, payload + 16U, 16U);
    return 1;
}

size_t xgw_encode_keepalive_payload(const xgw_keepalive_payload_t *keepalive, uint8_t *out, size_t out_cap) {
    if (keepalive == NULL || out == NULL || out_cap < 40U) {
        return 0U;
    }
    write_be64(out, keepalive->timestamp_us);
    write_be64(out + 8U, keepalive->ack.largest_acked);
    write_be64(out + 16U, keepalive->ack.ack_delay_us);
    write_be64(out + 24U, keepalive->ack.latest_rtt_us);
    write_be32(out + 32U, keepalive->ack.packets_acked);
    write_be32(out + 36U, keepalive->ack.packets_lost);
    write_be32(out + 40U, keepalive->ack.bytes_acked);
    write_be32(out + 44U, keepalive->ack.bytes_lost);
    return 48U;
}

int xgw_decode_keepalive_payload(const uint8_t *payload, size_t payload_len, xgw_keepalive_payload_t *keepalive) {
    if (payload == NULL || keepalive == NULL || payload_len < 48U) {
        return 0;
    }
    memset(keepalive, 0, sizeof(*keepalive));
    keepalive->timestamp_us = read_be64(payload);
    keepalive->ack.largest_acked = read_be64(payload + 8U);
    keepalive->ack.ack_delay_us = read_be64(payload + 16U);
    keepalive->ack.latest_rtt_us = read_be64(payload + 24U);
    keepalive->ack.packets_acked = read_be32(payload + 32U);
    keepalive->ack.packets_lost = read_be32(payload + 36U);
    keepalive->ack.bytes_acked = read_be32(payload + 40U);
    keepalive->ack.bytes_lost = read_be32(payload + 44U);
    return 1;
}
