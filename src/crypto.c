/* SHA-256 / HMAC / 基于摘要扩展的流加密实现。 */

#include "xgw_crypto.h"

#include <string.h>

static const uint32_t k256[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24U) |
           ((uint32_t) p[1] << 16U) |
           ((uint32_t) p[2] << 8U) |
           (uint32_t) p[3];
}

static void store_be64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t) (v >> 56U);
    p[1] = (uint8_t) (v >> 48U);
    p[2] = (uint8_t) (v >> 40U);
    p[3] = (uint8_t) (v >> 32U);
    p[4] = (uint8_t) (v >> 24U);
    p[5] = (uint8_t) (v >> 16U);
    p[6] = (uint8_t) (v >> 8U);
    p[7] = (uint8_t) v;
}

static void sha256_transform(xgw_sha256_ctx_t *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t i;

    for (i = 0; i < 16U; ++i) {
        w[i] = load_be32(block + (i * 4U));
    }
    for (i = 16U; i < 64U; ++i) {
        uint32_t s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
        uint32_t s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64U; ++i) {
        uint32_t s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k256[i] + w[i];
        uint32_t s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void xgw_sha256_init(xgw_sha256_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
}

void xgw_sha256_update(xgw_sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t offset = 0;
    if (ctx == NULL || data == NULL || len == 0U) {
        return;
    }
    ctx->total_len += len;
    while (offset < len) {
        size_t room = 64U - ctx->block_len;
        size_t take = len - offset;
        if (take > room) {
            take = room;
        }
        memcpy(ctx->block + ctx->block_len, data + offset, take);
        ctx->block_len += take;
        offset += take;
        if (ctx->block_len == 64U) {
            sha256_transform(ctx, ctx->block);
            ctx->block_len = 0U;
        }
    }
}

void xgw_sha256_final(xgw_sha256_ctx_t *ctx, uint8_t out[XGW_SHA256_LEN]) {
    uint64_t total_bits = ctx->total_len * 8U;
    size_t i;
    ctx->block[ctx->block_len++] = 0x80U;
    if (ctx->block_len > 56U) {
        while (ctx->block_len < 64U) {
            ctx->block[ctx->block_len++] = 0U;
        }
        sha256_transform(ctx, ctx->block);
        ctx->block_len = 0U;
    }
    while (ctx->block_len < 56U) {
        ctx->block[ctx->block_len++] = 0U;
    }
    store_be64(ctx->block + 56U, total_bits);
    sha256_transform(ctx, ctx->block);
    for (i = 0; i < 8U; ++i) {
        out[i * 4U] = (uint8_t) (ctx->state[i] >> 24U);
        out[i * 4U + 1U] = (uint8_t) (ctx->state[i] >> 16U);
        out[i * 4U + 2U] = (uint8_t) (ctx->state[i] >> 8U);
        out[i * 4U + 3U] = (uint8_t) ctx->state[i];
    }
}

void xgw_hmac_sha256(const uint8_t *key,
                     size_t key_len,
                     const uint8_t *data,
                     size_t data_len,
                     uint8_t out[XGW_SHA256_LEN]) {
    uint8_t k0[64];
    uint8_t tmp[XGW_SHA256_LEN];
    uint8_t ipad[64];
    uint8_t opad[64];
    xgw_sha256_ctx_t ctx;
    size_t i;

    memset(k0, 0, sizeof(k0));
    if (key_len > sizeof(k0)) {
        xgw_sha256_init(&ctx);
        xgw_sha256_update(&ctx, key, key_len);
        xgw_sha256_final(&ctx, tmp);
        memcpy(k0, tmp, sizeof(tmp));
    } else if (key != NULL && key_len > 0U) {
        memcpy(k0, key, key_len);
    }

    for (i = 0; i < sizeof(k0); ++i) {
        ipad[i] = (uint8_t) (k0[i] ^ 0x36U);
        opad[i] = (uint8_t) (k0[i] ^ 0x5cU);
    }

    xgw_sha256_init(&ctx);
    xgw_sha256_update(&ctx, ipad, sizeof(ipad));
    xgw_sha256_update(&ctx, data, data_len);
    xgw_sha256_final(&ctx, tmp);

    xgw_sha256_init(&ctx);
    xgw_sha256_update(&ctx, opad, sizeof(opad));
    xgw_sha256_update(&ctx, tmp, sizeof(tmp));
    xgw_sha256_final(&ctx, out);
}

void xgw_hash_expand_xor(const uint8_t *key,
                         size_t key_len,
                         uint64_t nonce,
                         uint8_t *data,
                         size_t len) {
    uint8_t material[96];
    uint8_t digest[XGW_SHA256_LEN];
    size_t offset = 0;
    uint32_t counter = 0;

    if (data == NULL || len == 0U || key == NULL || key_len == 0U) {
        return;
    }

    while (offset < len) {
        size_t take = len - offset;
        size_t i;
        memcpy(material, key, key_len > 64U ? 64U : key_len);
        memset(material + (key_len > 64U ? 64U : key_len), 0, 64U - (key_len > 64U ? 64U : key_len));
        store_be64(material + 64U, nonce);
        material[72] = (uint8_t) (counter >> 24U);
        material[73] = (uint8_t) (counter >> 16U);
        material[74] = (uint8_t) (counter >> 8U);
        material[75] = (uint8_t) counter;
        xgw_hmac_sha256(key, key_len, material, 76U, digest);
        if (take > sizeof(digest)) {
            take = sizeof(digest);
        }
        for (i = 0; i < take; ++i) {
            data[offset + i] ^= digest[i];
        }
        offset += take;
        ++counter;
    }
}

uint64_t xgw_fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    if (data == NULL) {
        return h;
    }
    for (i = 0; i < len; ++i) {
        h ^= (uint64_t) data[i];
        h *= 1099511628211ULL;
    }
    return h;
}
