#ifndef XGW_CRYPTO_H
#define XGW_CRYPTO_H

/* 轻量密码学基础设施：SHA-256、HMAC-SHA256 与基于摘要扩展的流加密。 */

#include <stddef.h>
#include <stdint.h>

#define XGW_SHA256_LEN 32

typedef struct xgw_sha256_ctx {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t block[64];
    size_t block_len;
} xgw_sha256_ctx_t;

void xgw_sha256_init(xgw_sha256_ctx_t *ctx);
void xgw_sha256_update(xgw_sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void xgw_sha256_final(xgw_sha256_ctx_t *ctx, uint8_t out[XGW_SHA256_LEN]);

void xgw_hmac_sha256(const uint8_t *key,
                     size_t key_len,
                     const uint8_t *data,
                     size_t data_len,
                     uint8_t out[XGW_SHA256_LEN]);

void xgw_hash_expand_xor(const uint8_t *key,
                         size_t key_len,
                         uint64_t nonce,
                         uint8_t *data,
                         size_t len);

uint64_t xgw_fnv1a64(const uint8_t *data, size_t len);

#endif
