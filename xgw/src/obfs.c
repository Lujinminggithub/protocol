/* 混淆层实现：使用随机 salt 和摘要扩展流，提供比简单异或更稳的包外观扰动。 */

#include "xgw_obfs.h"

#include "xgw_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void xgw_obfs_init(xgw_obfs_t *obfs, const char *mode, const char *key) {
    memset(obfs, 0, sizeof(*obfs));
    snprintf(obfs->mode, sizeof(obfs->mode), "%s", mode == NULL ? "" : mode);
    snprintf(obfs->key, sizeof(obfs->key), "%s", key == NULL ? "" : key);
    srand((unsigned int) time(NULL));
}

int xgw_obfs_enabled(const xgw_obfs_t *obfs) {
    return obfs != NULL &&
           obfs->mode[0] != '\0' &&
           strcmp(obfs->mode, "none") != 0 &&
           obfs->key[0] != '\0';
}

size_t xgw_obfs_encode(const xgw_obfs_t *obfs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    uint64_t nonce;
    size_t i;
    if (!xgw_obfs_enabled(obfs)) {
        if (out_cap < in_len) {
            return 0U;
        }
        memcpy(out, in, in_len);
        return in_len;
    }
    if (out_cap < in_len + XGW_OBFS_SALT_LEN) {
        return 0U;
    }
    nonce = ((uint64_t) rand() << 32U) ^ (uint64_t) rand();
    for (i = 0; i < XGW_OBFS_SALT_LEN; ++i) {
        out[i] = (uint8_t) (nonce >> ((7U - i) * 8U));
    }
    memcpy(out + XGW_OBFS_SALT_LEN, in, in_len);
    xgw_hash_expand_xor((const uint8_t *) obfs->key, strlen(obfs->key), nonce, out + XGW_OBFS_SALT_LEN, in_len);
    return in_len + XGW_OBFS_SALT_LEN;
}

size_t xgw_obfs_decode(const xgw_obfs_t *obfs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    uint64_t nonce = 0U;
    size_t i;
    if (!xgw_obfs_enabled(obfs)) {
        if (out_cap < in_len) {
            return 0U;
        }
        memcpy(out, in, in_len);
        return in_len;
    }
    if (in_len <= XGW_OBFS_SALT_LEN || out_cap < in_len - XGW_OBFS_SALT_LEN) {
        return 0U;
    }
    for (i = 0; i < XGW_OBFS_SALT_LEN; ++i) {
        nonce = (nonce << 8U) | in[i];
    }
    memcpy(out, in + XGW_OBFS_SALT_LEN, in_len - XGW_OBFS_SALT_LEN);
    xgw_hash_expand_xor((const uint8_t *) obfs->key, strlen(obfs->key), nonce, out, in_len - XGW_OBFS_SALT_LEN);
    return in_len - XGW_OBFS_SALT_LEN;
}
