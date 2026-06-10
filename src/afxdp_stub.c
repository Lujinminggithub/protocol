/* 非 Linux 环境下的 AF_XDP stub。 */

#ifndef __linux__
#include "xgw_afxdp.h"

#include <stdio.h>
#include <string.h>

int xgw_afxdp_open(xgw_afxdp_socket_t *sock, const char *device, uint32_t queue_id, uint16_t port, char *error, size_t error_len) {
    memset(sock, 0, sizeof(*sock));
    snprintf(sock->device, sizeof(sock->device), "%s", device == NULL ? "" : device);
    sock->queue_id = queue_id;
    sock->port = port;
    snprintf(error, error_len, "af_xdp binding is reserved for linux implementation in the next kernel-facing step");
    return 0;
}

int xgw_afxdp_recv(xgw_afxdp_socket_t *sock, uint8_t *buf, size_t buf_cap, size_t *out_len) {
    (void) sock;
    (void) buf;
    (void) buf_cap;
    (void) out_len;
    return 0;
}

int xgw_afxdp_send(xgw_afxdp_socket_t *sock, const uint8_t *buf, size_t buf_len) {
    (void) sock;
    (void) buf;
    (void) buf_len;
    return 0;
}

void xgw_afxdp_close(xgw_afxdp_socket_t *sock) {
    (void) sock;
}
#endif
