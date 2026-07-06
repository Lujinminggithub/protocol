#ifndef XGW_OBFS_H
#define XGW_OBFS_H

/* 参考 HY2 Salamander 思路的轻量 UDP 混淆层。 */

#include <stddef.h>
#include <stdint.h>

#define XGW_OBFS_SALT_LEN 8

typedef struct xgw_obfs {
    char mode[32];
    char key[128];
} xgw_obfs_t;

void xgw_obfs_init(xgw_obfs_t *obfs, const char *mode, const char *key);
int xgw_obfs_enabled(const xgw_obfs_t *obfs);
size_t xgw_obfs_encode(const xgw_obfs_t *obfs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);
size_t xgw_obfs_decode(const xgw_obfs_t *obfs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);

#endif
