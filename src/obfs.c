/* 轻量混淆层实现，参考 HY2 Salamander 思路，但保持当前实现简单可控。 */

#include "xgw_obfs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < len; ++i) {
        h ^= (uint64_t) data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

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
    uint8_t salt[XGW_OBFS_SALT_LEN];
    uint8_t material[256];
    uint64_t key;
    size_t i;
    if (!xgw_obfs_enabled(obfs)) {
        if (out_cap < in_len) {
            return 0;
        }
        memcpy(out, in, in_len);
        return in_len;
    }
    if (out_cap < in_len + XGW_OBFS_SALT_LEN) {
        return 0;
    }
    for (i = 0; i < XGW_OBFS_SALT_LEN; ++i) {
        salt[i] = (uint8_t) (rand() & 0xff);
        out[i] = salt[i];
    }
    snprintf((char *) material, sizeof(material), "%s", obfs->key);
    memcpy(material + strlen(obfs->key), salt, XGW_OBFS_SALT_LEN);
    key = fnv1a64(material, strlen(obfs->key) + XGW_OBFS_SALT_LEN);
    for (i = 0; i < in_len; ++i) {
        out[i + XGW_OBFS_SALT_LEN] = in[i] ^ ((uint8_t *) &key)[i % sizeof(key)];
    }
    return in_len + XGW_OBFS_SALT_LEN;
}

size_t xgw_obfs_decode(const xgw_obfs_t *obfs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    uint8_t material[256];
    uint64_t key;
    size_t i;
    if (!xgw_obfs_enabled(obfs)) {
        if (out_cap < in_len) {
            return 0;
        }
        memcpy(out, in, in_len);
        return in_len;
    }
    if (in_len <= XGW_OBFS_SALT_LEN || out_cap < in_len - XGW_OBFS_SALT_LEN) {
        return 0;
    }
    snprintf((char *) material, sizeof(material), "%s", obfs->key);
    memcpy(material + strlen(obfs->key), in, XGW_OBFS_SALT_LEN);
    key = fnv1a64(material, strlen(obfs->key) + XGW_OBFS_SALT_LEN);
    for (i = XGW_OBFS_SALT_LEN; i < in_len; ++i) {
        out[i - XGW_OBFS_SALT_LEN] = in[i] ^ ((uint8_t *) &key)[(i - XGW_OBFS_SALT_LEN) % sizeof(key)];
    }
    return in_len - XGW_OBFS_SALT_LEN;
}
