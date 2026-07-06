#ifndef XGW_SECURITY_H
#define XGW_SECURITY_H

/* 会话安全层：认证 token 校验、会话密钥派生、载荷加密与完整性校验。 */

#include "xgw_protocol.h"

#include <stddef.h>
#include <stdint.h>

#define XGW_SECURITY_TAG_LEN 16
#define XGW_SECURITY_NONCE_LEN 8

typedef struct xgw_security_material {
    uint8_t static_key[32];
    uint8_t tx_key[32];
    uint8_t rx_key[32];
    uint64_t local_nonce;
    uint64_t peer_nonce;
    int static_ready;
    int session_ready;
} xgw_security_material_t;

void xgw_security_init(xgw_security_material_t *sec, const char *auth_token);
void xgw_security_begin_handshake(xgw_security_material_t *sec, uint64_t local_nonce);
void xgw_security_complete_handshake(xgw_security_material_t *sec,
                                     uint64_t local_nonce,
                                     uint64_t peer_nonce,
                                     int initiator);
void xgw_security_auth_tag(const xgw_security_material_t *sec,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint8_t out[16]);
int xgw_security_verify_auth_tag(const xgw_security_material_t *sec,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const uint8_t tag[16]);
size_t xgw_security_seal_payload(const xgw_security_material_t *sec,
                                 const uint8_t *plain,
                                 size_t plain_len,
                                 uint64_t packet_nonce,
                                 uint8_t *out,
                                 size_t out_cap);
size_t xgw_security_open_payload(const xgw_security_material_t *sec,
                                 const uint8_t *sealed,
                                 size_t sealed_len,
                                 uint64_t packet_nonce,
                                 uint8_t *out,
                                 size_t out_cap);
void xgw_security_debug_summary(const xgw_security_material_t *sec, char *out, size_t out_len);

#endif
