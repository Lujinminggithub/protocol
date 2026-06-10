#ifndef XGW_PACKET_H
#define XGW_PACKET_H

#include <stddef.h>
#include <stdint.h>

/* 单个接收包的最小表示。 */
typedef struct xgw_packet {
    uint8_t data[65535];
    size_t data_len;
    char remote_host[64];
    uint16_t remote_port;
} xgw_packet_t;

#endif
