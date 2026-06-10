/* UDP 传输实现。 */

#include "xgw_transport.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_arg_t;
static int xgw_winsock_init(void) {
    static int initialized = 0;
    if (!initialized) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return 0;
        }
        initialized = 1;
    }
    return 1;
}
#define xgw_close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef socklen_t socklen_arg_t;
typedef int SOCKET;
static int xgw_winsock_init(void) { return 1; }
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define xgw_close_socket close
#endif

static int fill_sockaddr(const char *host, uint16_t port, struct sockaddr_in *out) {
    char port_text[16];
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    snprintf(port_text, sizeof(port_text), "%u", port);
    if (host == NULL || host[0] == '\0' || strcmp(host, "*") == 0) {
        out->sin_addr.s_addr = htonl(INADDR_ANY);
        out->sin_port = htons(port);
        return 1;
    }
    if (inet_pton(AF_INET, host, &out->sin_addr) != 1) {
        return 0;
    }
    out->sin_port = htons(port);
    return 1;
}

int xgw_udp_listen(xgw_udp_socket_t *sock, const char *bind_host, uint16_t port) {
    struct sockaddr_in addr;
    int fd;
    SOCKET raw_fd;
    memset(sock, 0, sizeof(*sock));
    sock->fd = -1;
    if (!xgw_winsock_init()) {
        return 0;
    }
    if (!fill_sockaddr(bind_host, port, &addr)) {
        return 0;
    }
    raw_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (raw_fd == INVALID_SOCKET) {
        return 0;
    }
    fd = (int) raw_fd;
    if (bind(fd, (struct sockaddr *) &addr, (socklen_arg_t) sizeof(addr)) == SOCKET_ERROR) {
        xgw_close_socket(fd);
        return 0;
    }
    sock->fd = fd;
    sock->initialized = 1;
    return 1;
}

int xgw_udp_set_remote(xgw_udp_socket_t *sock, const char *host, uint16_t port) {
    if (sock == NULL) {
        return 0;
    }
    snprintf(sock->remote_host, sizeof(sock->remote_host), "%s", host == NULL ? "" : host);
    sock->remote_port = port;
    return 1;
}

int xgw_udp_recv(xgw_udp_socket_t *sock, xgw_packet_t *packet, int timeout_ms) {
    struct sockaddr_in remote;
    socklen_arg_t remote_len = (socklen_arg_t) sizeof(remote);
    fd_set rfds;
    int ready;
    struct timeval tv;
    if (sock == NULL || !sock->initialized) {
        return -1;
    }
    FD_ZERO(&rfds);
    FD_SET((unsigned int) sock->fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ready = select(sock->fd + 1, &rfds, NULL, NULL, timeout_ms >= 0 ? &tv : NULL);
    if (ready <= 0) {
        return 0;
    }
    packet->data_len = recvfrom(sock->fd,
                                (char *) packet->data,
                                (int) sizeof(packet->data),
                                0,
                                (struct sockaddr *) &remote,
                                &remote_len);
    if ((int) packet->data_len <= 0) {
        return -1;
    }
    inet_ntop(AF_INET, &remote.sin_addr, packet->remote_host, (socklen_arg_t) sizeof(packet->remote_host));
    packet->remote_port = ntohs(remote.sin_port);
    return 1;
}

/* 发送一个 UDP 包到指定地址，或发送到默认远端。 */
int xgw_udp_send(xgw_udp_socket_t *sock, const uint8_t *data, size_t data_len, const char *host, uint16_t port) {
    struct sockaddr_in remote;
    const char *target_host = host;
    uint16_t target_port = port;
    if (sock == NULL || !sock->initialized) {
        return 0;
    }
    if (target_host == NULL || target_host[0] == '\0') {
        target_host = sock->remote_host;
        target_port = sock->remote_port;
    }
    if (!fill_sockaddr(target_host, target_port, &remote)) {
        return 0;
    }
    return sendto(sock->fd,
                  (const char *) data,
                  (int) data_len,
                  0,
                  (struct sockaddr *) &remote,
                  (socklen_arg_t) sizeof(remote)) == (int) data_len;
}

int xgw_udp_set_buffers(xgw_udp_socket_t *sock, int rcvbuf_bytes, int sndbuf_bytes) {
    if (sock == NULL || !sock->initialized) {
        return 0;
    }
    if (rcvbuf_bytes > 0) {
        if (setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, (const char *) &rcvbuf_bytes, sizeof(rcvbuf_bytes)) != 0) {
            return 0;
        }
    }
    if (sndbuf_bytes > 0) {
        if (setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, (const char *) &sndbuf_bytes, sizeof(sndbuf_bytes)) != 0) {
            return 0;
        }
    }
    return 1;
}

void xgw_udp_close(xgw_udp_socket_t *sock) {
    if (sock != NULL && sock->initialized) {
        xgw_close_socket(sock->fd);
        sock->fd = -1;
        sock->initialized = 0;
    }
}
