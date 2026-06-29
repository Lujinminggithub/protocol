/* UDP 传输实现。 */

#ifndef _WIN32
/* Linux 需要 _GNU_SOURCE 才能暴露 IP_MTU_DISCOVER / IP_PMTUDISC_DO / IP_MTU。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

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
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
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

static int g_xgw_udp_last_error_code = 0;
static char g_xgw_udp_last_error_stage[32] = "";
static char g_xgw_udp_last_error_host[64] = "";
static uint16_t g_xgw_udp_last_error_port = 0U;

static void xgw_udp_set_last_error(const char *stage, const char *host, uint16_t port, int code) {
    g_xgw_udp_last_error_code = code;
    snprintf(g_xgw_udp_last_error_stage, sizeof(g_xgw_udp_last_error_stage), "%s", stage == NULL ? "" : stage);
    snprintf(g_xgw_udp_last_error_host, sizeof(g_xgw_udp_last_error_host), "%s", host == NULL ? "" : host);
    g_xgw_udp_last_error_port = port;
}

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
#ifdef _WIN32
    DWORD bytes = 0;
    BOOL new_behavior = FALSE;
#endif
    memset(sock, 0, sizeof(*sock));
    sock->fd = -1;
    xgw_udp_set_last_error("", bind_host, port, 0);
    if (!xgw_winsock_init()) {
        xgw_udp_set_last_error("winsock_init", bind_host, port, -1);
        return 0;
    }
    if (!fill_sockaddr(bind_host, port, &addr)) {
        xgw_udp_set_last_error("fill_sockaddr", bind_host, port, -2);
        return 0;
    }
    raw_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (raw_fd == INVALID_SOCKET) {
#ifdef _WIN32
        xgw_udp_set_last_error("socket", bind_host, port, WSAGetLastError());
#else
        xgw_udp_set_last_error("socket", bind_host, port, errno);
#endif
        return 0;
    }
    fd = (int) raw_fd;
    if (bind(fd, (struct sockaddr *) &addr, (socklen_arg_t) sizeof(addr)) == SOCKET_ERROR) {
#ifdef _WIN32
        xgw_udp_set_last_error("bind", bind_host, port, WSAGetLastError());
#else
        xgw_udp_set_last_error("bind", bind_host, port, errno);
#endif
        xgw_close_socket(fd);
        return 0;
    }
#ifdef _WIN32
    (void) WSAIoctl(fd,
                    SIO_UDP_CONNRESET,
                    &new_behavior,
                    sizeof(new_behavior),
                    NULL,
                    0,
                    &bytes,
                    NULL,
                    NULL);
#endif
    sock->fd = fd;
    sock->initialized = 1;
    return 1;
}

int xgw_udp_last_error_code(void) {
    return g_xgw_udp_last_error_code;
}

const char *xgw_udp_last_error_stage(void) {
    return g_xgw_udp_last_error_stage;
}

const char *xgw_udp_last_error_host(void) {
    return g_xgw_udp_last_error_host;
}

uint16_t xgw_udp_last_error_port(void) {
    return g_xgw_udp_last_error_port;
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
#ifdef _WIN32
        fprintf(stderr, "xgw_udp_recv fail ready=%d data_len=%d wsa=%d\n", ready, (int) packet->data_len, WSAGetLastError());
#else
        fprintf(stderr, "xgw_udp_recv fail ready=%d data_len=%d errno=%d (%s)\n", ready, (int) packet->data_len, errno, strerror(errno));
#endif
        return -1;
    }
    inet_ntop(AF_INET, &remote.sin_addr, packet->remote_host, (socklen_arg_t) sizeof(packet->remote_host));
    packet->remote_port = ntohs(remote.sin_port);
    return 1;
}

/* 发送一个 UDP 包到指定地址，或发送到默认远端。 */
int xgw_udp_send_ex(xgw_udp_socket_t *sock,
                    const uint8_t *data,
                    size_t data_len,
                    const char *host,
                    uint16_t port,
                    int *out_sent_bytes,
                    int *out_error_code) {
    struct sockaddr_in remote;
    const char *target_host = host;
    uint16_t target_port = port;
    int sent_bytes;
    int ok;
    if (sock == NULL || !sock->initialized) {
        if (out_sent_bytes != NULL) {
            *out_sent_bytes = -1;
        }
        if (out_error_code != NULL) {
            *out_error_code = -1;
        }
        return 0;
    }
    if (target_host == NULL || target_host[0] == '\0') {
        target_host = sock->remote_host;
        target_port = sock->remote_port;
    }
    if (!fill_sockaddr(target_host, target_port, &remote)) {
        if (out_sent_bytes != NULL) {
            *out_sent_bytes = -1;
        }
        if (out_error_code != NULL) {
            *out_error_code = -2;
        }
        return 0;
    }
    sent_bytes = (int) sendto(sock->fd,
                              (const char *) data,
                              (int) data_len,
                              0,
                              (struct sockaddr *) &remote,
                              (socklen_arg_t) sizeof(remote));
#ifdef _WIN32
    if (out_error_code != NULL) {
        *out_error_code = sent_bytes >= 0 ? 0 : WSAGetLastError();
    }
#else
    if (out_error_code != NULL) {
        *out_error_code = sent_bytes >= 0 ? 0 : errno;
    }
#endif
    if (out_sent_bytes != NULL) {
        *out_sent_bytes = sent_bytes;
    }
    ok = sent_bytes == (int) data_len;
    return ok;
}

int xgw_udp_send(xgw_udp_socket_t *sock, const uint8_t *data, size_t data_len, const char *host, uint16_t port) {
    return xgw_udp_send_ex(sock, data, data_len, host, port, NULL, NULL);
}

/* 打开路径 MTU 发现：在 socket 上置 DF（Don't Fragment）位，使超过路径 PMTU 的包
 * 被丢弃并产生 EMSGSIZE / ICMP「需要分片」，从而由上层做 PMTUD 探测与回退，
 * 而不是被中间设备静默分片（更易丢、重组失败）。返回 1 表示已启用。 */
int xgw_udp_enable_pmtud(xgw_udp_socket_t *sock) {
    if (sock == NULL || !sock->initialized) {
        return 0;
    }
#if defined(_WIN32)
    {
        DWORD on = 1; /* IP_DONTFRAGMENT */
#ifndef IP_DONTFRAGMENT
#define IP_DONTFRAGMENT 14
#endif
        if (setsockopt(sock->fd, IPPROTO_IP, IP_DONTFRAGMENT, (const char *) &on, sizeof(on)) != 0) {
            return 0;
        }
        return 1;
    }
#elif defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
    {
        int val = IP_PMTUDISC_DO; /* 内核维护每路径 PMTU，置 DF 并在收到 ICMP 时更新 */
        if (setsockopt(sock->fd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val)) != 0) {
            return 0;
        }
        return 1;
    }
#elif defined(IP_DONTFRAG)
    {
        int on = 1; /* BSD/macOS */
        if (setsockopt(sock->fd, IPPROTO_IP, IP_DONTFRAG, &on, sizeof(on)) != 0) {
            return 0;
        }
        return 1;
    }
#else
    return 0;
#endif
}

/* 查询内核已发现的路径 MTU（字节）。Linux 走 IP_MTU；其他平台暂不可查返回 0。
 * 仅在 socket 已 connect 或已有路径状态时有效。 */
uint32_t xgw_udp_query_pmtu(xgw_udp_socket_t *sock) {
#if !defined(_WIN32) && defined(IP_MTU)
    int mtu = 0;
    socklen_arg_t len = (socklen_arg_t) sizeof(mtu);
    if (sock == NULL || !sock->initialized) {
        return 0U;
    }
    if (getsockopt(sock->fd, IPPROTO_IP, IP_MTU, &mtu, &len) != 0) {
        return 0U;
    }
    return mtu > 0 ? (uint32_t) mtu : 0U;
#else
    (void) sock;
    return 0U;
#endif
}

/* 判断上次 send 的错误码是否为「报文过大」（EMSGSIZE / WSAEMSGSIZE），
 * 即超过路径 PMTU 且置了 DF。上层据此把有效负载下调一档并重试。 */
int xgw_udp_error_is_too_big(int error_code) {
#if defined(_WIN32)
    return error_code == WSAEMSGSIZE;
#else
    return error_code == EMSGSIZE;
#endif
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
