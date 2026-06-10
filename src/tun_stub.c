/* 非 Linux 环境下的 TUN stub。 */

#ifndef __linux__
#include "xgw_tun.h"

#include <stdio.h>
#include <string.h>

int xgw_tun_open(xgw_tun_device_t *dev, const char *name, const char *addr, uint32_t mtu, char *error, size_t error_len) {
    (void) dev;
    (void) name;
    (void) addr;
    (void) mtu;
    snprintf(error, error_len, "tun is only implemented on linux in this stage");
    return 0;
}

int xgw_tun_read(xgw_tun_device_t *dev, uint8_t *buf, size_t buf_cap, size_t *out_len) {
    (void) dev;
    (void) buf;
    (void) buf_cap;
    (void) out_len;
    return 0;
}

int xgw_tun_write(xgw_tun_device_t *dev, const uint8_t *buf, size_t buf_len) {
    (void) dev;
    (void) buf;
    (void) buf_len;
    return 0;
}

void xgw_tun_close(xgw_tun_device_t *dev) {
    (void) dev;
}
#endif
