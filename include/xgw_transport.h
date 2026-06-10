#ifndef XGW_TRANSPORT_H
#define XGW_TRANSPORT_H

/* 传输层抽象，目前以 UDP 为主。 */

#include "xgw_packet.h"

#include <stddef.h>
#include <stdint.h>

/* UDP socket 状态。 */
typedef struct xgw_udp_socket {
    int fd;
    int initialized;
    char remote_host[64];
    uint16_t remote_port;
} xgw_udp_socket_t;

/* 绑定并监听本地 UDP 端口。 */
int xgw_udp_listen(xgw_udp_socket_t *sock, const char *bind_host, uint16_t port);
/* 设置默认远端地址。 */
int xgw_udp_set_remote(xgw_udp_socket_t *sock, const char *host, uint16_t port);
/* 接收一个 UDP 包。 */
int xgw_udp_recv(xgw_udp_socket_t *sock, xgw_packet_t *packet, int timeout_ms);
/* 发送一个 UDP 包。 */
int xgw_udp_send(xgw_udp_socket_t *sock, const uint8_t *data, size_t data_len, const char *host, uint16_t port);
/* 调整 UDP socket 读写缓冲区。 */
int xgw_udp_set_buffers(xgw_udp_socket_t *sock, int rcvbuf_bytes, int sndbuf_bytes);
/* 关闭 UDP socket。 */
void xgw_udp_close(xgw_udp_socket_t *sock);

#endif
