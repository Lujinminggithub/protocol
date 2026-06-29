/* 会话安全层实现：静态 token 认证、双向 nonce 派生与带标签加解密。 */

#include "xgw_security.h"

#include "xgw_crypto.h"

#include <stdio.h>
#include <string.h>

static void derive_key_pair(xgw_security_material_t *sec,
                            const char *label,
                            uint64_t local_nonce,
                            uint64_t peer_nonce,
                            uint8_t out[32]) {
    uint8_t material[64];
    size_t label_len = strlen(label);
    memset(material, 0, sizeof(material));
    memcpy(material, label, label_len > 16U ? 16U : label_len);
    material[16] = (uint8_t) (local_nonce >> 56U);
    material[17] = (uint8_t) (local_nonce >> 48U);
    material[18] = (uint8_t) (local_nonce >> 40U);
    material[19] = (uint8_t) (local_nonce >> 32U);
    material[20] = (uint8_t) (local_nonce >> 24U);
    material[21] = (uint8_t) (local_nonce >> 16U);
    material[22] = (uint8_t) (local_nonce >> 8U);
    material[23] = (uint8_t) local_nonce;
    material[24] = (uint8_t) (peer_nonce >> 56U);
    material[25] = (uint8_t) (peer_nonce >> 48U);
    material[26] = (uint8_t) (peer_nonce >> 40U);
    material[27] = (uint8_t) (peer_nonce >> 32U);
    material[28] = (uint8_t) (peer_nonce >> 24U);
    material[29] = (uint8_t) (peer_nonce >> 16U);
    material[30] = (uint8_t) (peer_nonce >> 8U);
    material[31] = (uint8_t) peer_nonce;
    xgw_hmac_sha256(sec->static_key, sizeof(sec->static_key), material, 32U, out);
}

void xgw_security_init(xgw_security_material_t *sec, const char *auth_token) {
    memset(sec, 0, sizeof(*sec));
    if (auth_token != NULL && auth_token[0] != '\0') {
        xgw_hmac_sha256((const uint8_t *) "xgw-static", 10U,
                        (const uint8_t *) auth_token,
                        strlen(auth_token),
                        sec->static_key);
        sec->static_ready = 1;
    }
}

void xgw_security_begin_handshake(xgw_security_material_t *sec, uint64_t local_nonce) {
    if (sec == NULL) {
        return;
    }
    sec->local_nonce = local_nonce;
}

void xgw_security_complete_handshake(xgw_security_material_t *sec,
                                     uint64_t local_nonce,
                                     uint64_t peer_nonce,
                                     int initiator) {
    if (sec == NULL || !sec->static_ready) {
        return;
    }
    sec->local_nonce = local_nonce;
    sec->peer_nonce = peer_nonce;
    if (initiator) {
        derive_key_pair(sec, "tx", local_nonce, peer_nonce, sec->tx_key);
        derive_key_pair(sec, "rx", peer_nonce, local_nonce, sec->rx_key);
    } else {
        derive_key_pair(sec, "rx", local_nonce, peer_nonce, sec->tx_key);
        derive_key_pair(sec, "tx", peer_nonce, local_nonce, sec->rx_key);
    }
    sec->session_ready = 1;
}

void xgw_security_auth_tag(const xgw_security_material_t *sec,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint8_t out[16]) {
    uint8_t full[32];
    memset(out, 0, 16U);
    if (sec == NULL || !sec->static_ready) {
        return;
    }
    xgw_hmac_sha256(sec->static_key, sizeof(sec->static_key), payload, payload_len, full);
    memcpy(out, full, 16U);
}

int xgw_security_verify_auth_tag(const xgw_security_material_t *sec,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const uint8_t tag[16]) {
    uint8_t expected[16];
    size_t i;
    xgw_security_auth_tag(sec, payload, payload_len, expected);
    for (i = 0; i < 16U; ++i) {
        if (expected[i] != tag[i]) {
            return 0;
        }
    }
    return 1;
}

size_t xgw_security_seal_payload(const xgw_security_material_t *sec,
                                 const uint8_t *plain,
                                 size_t plain_len,
                                 uint64_t packet_nonce,
                                 uint8_t *out,
                                 size_t out_cap) {
    uint8_t tag[32];
    uint8_t nonce_bytes[8];
    size_t i;
    if (sec == NULL || out == NULL || plain == NULL || !sec->session_ready) {
        return 0U;
    }
    if (out_cap < XGW_SECURITY_NONCE_LEN + plain_len + XGW_SECURITY_TAG_LEN) {
        return 0U;
    }
    for (i = 0; i < 8U; ++i) {
        nonce_bytes[i] = (uint8_t) (packet_nonce >> ((7U - i) * 8U));
        out[i] = nonce_bytes[i];
    }
    memcpy(out + 8U, plain, plain_len);
    xgw_hash_expand_xor(sec->tx_key, sizeof(sec->tx_key), packet_nonce, out + 8U, plain_len);
    xgw_hmac_sha256(sec->tx_key, sizeof(sec->tx_key), out, 8U + plain_len, tag);
    memcpy(out + 8U + plain_len, tag, XGW_SECURITY_TAG_LEN);
    if (packet_nonce <= 24U) {
        printf("security.seal nonce=%llu tx=%02x%02x%02x%02x tag=%02x%02x%02x%02x len=%zu\n",
               (unsigned long long) packet_nonce,
               sec->tx_key[0], sec->tx_key[1], sec->tx_key[2], sec->tx_key[3],
               tag[0], tag[1], tag[2], tag[3],
               plain_len);
        fflush(stdout);
    }
    return 8U + plain_len + XGW_SECURITY_TAG_LEN;
}

size_t xgw_security_open_payload(const xgw_security_material_t *sec,
                                 const uint8_t *sealed,
                                 size_t sealed_len,
                                 uint64_t packet_nonce,
                                 uint8_t *out,
                                 size_t out_cap) {
    uint8_t tag[32];
    size_t plain_len;
    size_t i;
    if (sec == NULL || sealed == NULL || out == NULL || !sec->session_ready) {
        return 0U;
    }
    if (sealed_len <= XGW_SECURITY_NONCE_LEN + XGW_SECURITY_TAG_LEN) {
        return 0U;
    }
    plain_len = sealed_len - XGW_SECURITY_NONCE_LEN - XGW_SECURITY_TAG_LEN;
    if (out_cap < plain_len) {
        return 0U;
    }
    xgw_hmac_sha256(sec->rx_key, sizeof(sec->rx_key), sealed, sealed_len - XGW_SECURITY_TAG_LEN, tag);
    for (i = 0; i < XGW_SECURITY_TAG_LEN; ++i) {
        if (tag[i] != sealed[sealed_len - XGW_SECURITY_TAG_LEN + i]) {
            printf("security.open.fail nonce=%llu rx=%02x%02x%02x%02x exp=%02x%02x%02x%02x got=%02x%02x%02x%02x len=%zu\n",
                   (unsigned long long) packet_nonce,
                   sec->rx_key[0], sec->rx_key[1], sec->rx_key[2], sec->rx_key[3],
                   tag[0], tag[1], tag[2], tag[3],
                   sealed[sealed_len - XGW_SECURITY_TAG_LEN + 0U],
                   sealed[sealed_len - XGW_SECURITY_TAG_LEN + 1U],
                   sealed[sealed_len - XGW_SECURITY_TAG_LEN + 2U],
                   sealed[sealed_len - XGW_SECURITY_TAG_LEN + 3U],
                   sealed_len);
            fflush(stdout);
            return 0U;
        }
    }
    memcpy(out, sealed + XGW_SECURITY_NONCE_LEN, plain_len);
    xgw_hash_expand_xor(sec->rx_key, sizeof(sec->rx_key), packet_nonce, out, plain_len);
    return plain_len;
}

void xgw_security_debug_summary(const xgw_security_material_t *sec, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (sec == NULL) {
        snprintf(out, out_len, "sec=null");
        return;
    }
    snprintf(out,
             out_len,
             "static=%d session=%d local_nonce=%llu peer_nonce=%llu tx=%02x%02x%02x%02x rx=%02x%02x%02x%02x",
             sec->static_ready,
             sec->session_ready,
             (unsigned long long) sec->local_nonce,
             (unsigned long long) sec->peer_nonce,
             sec->tx_key[0], sec->tx_key[1], sec->tx_key[2], sec->tx_key[3],
             sec->rx_key[0], sec->rx_key[1], sec->rx_key[2], sec->rx_key[3]);
}
