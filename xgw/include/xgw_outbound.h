#ifndef XGW_OUTBOUND_H
#define XGW_OUTBOUND_H

/* 出站链实现：direct / fixed / socks5 / http。 */

#include "xgw_packet.h"

#include <stddef.h>
#include <stdint.h>

typedef enum xgw_outbound_type {
    XGW_OUTBOUND_DIRECT = 0,
    XGW_OUTBOUND_FIXED,
    XGW_OUTBOUND_SOCKS5,
    XGW_OUTBOUND_HTTP
} xgw_outbound_type_t;

typedef struct xgw_outbound {
    xgw_outbound_type_t type;
    char name[64];
    char host[64];
    uint16_t port;
    char username[64];
    char password[64];
    int tcp_fd;
    int udp_fd;
    char udp_relay_host[64];
    uint16_t udp_relay_port;
    char current_target_host[64];
    uint16_t current_target_port;
} xgw_outbound_t;

typedef struct xgw_outbound_target {
    const char *host;
    uint16_t port;
} xgw_outbound_target_t;

int xgw_outbound_parse_type(const char *value, xgw_outbound_type_t *type);
const char *xgw_outbound_type_name(xgw_outbound_type_t type);
int xgw_outbound_open(xgw_outbound_t *outbound,
                      xgw_outbound_type_t type,
                      const char *name,
                      const char *host,
                      uint16_t port,
                      const char *username,
                      const char *password,
                      char *error,
                      size_t error_len);
int xgw_outbound_connect_tcp(xgw_outbound_t *outbound, const char *target_host, uint16_t target_port, char *error, size_t error_len);
int xgw_outbound_send_tcp(xgw_outbound_t *outbound, const uint8_t *data, size_t data_len);
int xgw_outbound_recv_tcp(xgw_outbound_t *outbound, uint8_t *buf, size_t buf_cap, int timeout_ms);
int xgw_outbound_send(xgw_outbound_t *outbound, const uint8_t *data, size_t data_len, const char *host, uint16_t port);
int xgw_outbound_recv(xgw_outbound_t *outbound, xgw_packet_t *packet, int timeout_ms);
void xgw_outbound_close(xgw_outbound_t *outbound);

#endif
