/* 出站链实现：direct / fixed / socks5 / http。 */

#include "xgw_outbound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_arg_t;
static int xgw_socket_init(void) {
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
static int xgw_socket_init(void) { return 1; }
#define xgw_close_socket close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

static void set_error(char *error, size_t error_len, const char *text) {
    if (error_len > 0U) {
        snprintf(error, error_len, "%s", text == NULL ? "" : text);
    }
}

static void stage_log(const char *stage, const char *detail) {
    printf("outbound.stage %s %s\n", stage, detail == NULL ? "" : detail);
    fflush(stdout);
}

static int fill_addr(const char *host, uint16_t port, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    return inet_pton(AF_INET, host, &addr->sin_addr) == 1;
}

static int tcp_connect_ipv4(const char *host, uint16_t port) {
    int fd;
    SOCKET raw_fd;
    struct sockaddr_in addr;
    if (!fill_addr(host, port, &addr)) {
        return -1;
    }
    raw_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (raw_fd == INVALID_SOCKET) {
        return -1;
    }
    fd = (int) raw_fd;
    if (connect(fd, (struct sockaddr *) &addr, (socklen_arg_t) sizeof(addr)) == SOCKET_ERROR) {
        xgw_close_socket(fd);
        return -1;
    }
    return fd;
}

static int udp_connect_ipv4(const char *host, uint16_t port) {
    int fd;
    SOCKET raw_fd;
    struct sockaddr_in addr;
    if (!fill_addr(host, port, &addr)) {
        return -1;
    }
    raw_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (raw_fd == INVALID_SOCKET) {
        return -1;
    }
    fd = (int) raw_fd;
    if (connect(fd, (struct sockaddr *) &addr, (socklen_arg_t) sizeof(addr)) == SOCKET_ERROR) {
        xgw_close_socket(fd);
        return -1;
    }
    return fd;
}

static int tcp_send_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, (const char *) buf + sent, (int) (len - sent), 0);
        if (n <= 0) {
            return 0;
        }
        sent += (size_t) n;
    }
    return 1;
}

static int tcp_recv_exact(int fd, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(fd, (char *) buf + got, (int) (len - got), 0);
        if (n <= 0) {
            return 0;
        }
        got += (size_t) n;
    }
    return 1;
}

static int wait_fd_readable(int fd, int timeout_ms) {
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET((unsigned int) fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, &rfds, NULL, NULL, timeout_ms >= 0 ? &tv : NULL) > 0;
}

static void build_socks5_addr(const char *host, uint16_t port, uint8_t *buf, size_t *out_len) {
    uint8_t ipv4[4];
    if (inet_pton(AF_INET, host, ipv4) == 1) {
        buf[0] = 0x01;
        memcpy(buf + 1, ipv4, 4);
        buf[5] = (uint8_t) (port >> 8U);
        buf[6] = (uint8_t) port;
        *out_len = 7;
        return;
    }
    buf[0] = 0x03;
    buf[1] = (uint8_t) strlen(host);
    memcpy(buf + 2, host, strlen(host));
    buf[2 + strlen(host)] = (uint8_t) (port >> 8U);
    buf[3 + strlen(host)] = (uint8_t) port;
    *out_len = 4 + strlen(host);
}

static int socks5_negotiate(xgw_outbound_t *outbound, char *error, size_t error_len) {
    uint8_t req[512];
    uint8_t resp[512];
    size_t req_len = 0;

    stage_log("socks5.negotiate.start", outbound->host);
    outbound->tcp_fd = tcp_connect_ipv4(outbound->host, outbound->port);
    if (outbound->tcp_fd < 0) {
        set_error(error, error_len, "socks5 tcp connect failed");
        return 0;
    }
    stage_log("socks5.negotiate.connected", outbound->host);

    req[0] = 0x05;
    if (outbound->username[0] != '\0' && outbound->password[0] != '\0') {
        req[1] = 2;
        req[2] = 0x00;
        req[3] = 0x02;
        req_len = 4;
    } else {
        req[1] = 1;
        req[2] = 0x00;
        req_len = 3;
    }
    if (!tcp_send_all(outbound->tcp_fd, req, req_len) || !tcp_recv_exact(outbound->tcp_fd, resp, 2)) {
        set_error(error, error_len, "socks5 negotiation failed");
        return 0;
    }
    stage_log("socks5.negotiate.method", resp[1] == 0x02 ? "userpass" : (resp[1] == 0x00 ? "none" : "other"));
    if (resp[1] == 0x02) {
        size_t ulen = strlen(outbound->username);
        size_t plen = strlen(outbound->password);
        req[0] = 0x01;
        req[1] = (uint8_t) ulen;
        memcpy(req + 2, outbound->username, ulen);
        req[2 + ulen] = (uint8_t) plen;
        memcpy(req + 3 + ulen, outbound->password, plen);
        req_len = 3 + ulen + plen;
        if (!tcp_send_all(outbound->tcp_fd, req, req_len) || !tcp_recv_exact(outbound->tcp_fd, resp, 2) || resp[1] != 0x00) {
            set_error(error, error_len, "socks5 username/password auth failed");
            return 0;
        }
        stage_log("socks5.auth.ok", outbound->username);
    } else if (resp[1] != 0x00) {
        set_error(error, error_len, "socks5 unsupported auth method");
        return 0;
    }
    stage_log("socks5.negotiate.done", outbound->host);
    return 1;
}

static int socks5_udp_associate(xgw_outbound_t *outbound, char *error, size_t error_len) {
    uint8_t req[32];
    uint8_t resp[512];
    size_t addr_len;
    uint16_t relay_port = 0;
    char relay_host[64];
    stage_log("socks5.udp_associate.start", outbound->host);
    req[0] = 0x05;
    req[1] = 0x03;
    req[2] = 0x00;
    build_socks5_addr("0.0.0.0", 0, req + 3, &addr_len);
    if (!tcp_send_all(outbound->tcp_fd, req, 3 + addr_len)) {
        set_error(error, error_len, "socks5 udp associate request failed");
        return 0;
    }
    if (!tcp_recv_exact(outbound->tcp_fd, resp, 4)) {
        set_error(error, error_len, "socks5 udp associate reply failed");
        return 0;
    }
    if (resp[1] != 0x00) {
        set_error(error, error_len, "socks5 udp associate rejected");
        return 0;
    }
    if (resp[3] == 0x01) {
        if (!tcp_recv_exact(outbound->tcp_fd, resp + 4, 6)) {
            set_error(error, error_len, "socks5 udp relay addr read failed");
            return 0;
        }
        snprintf(relay_host, sizeof(relay_host), "%u.%u.%u.%u", resp[4], resp[5], resp[6], resp[7]);
        relay_port = (uint16_t) ((resp[8] << 8U) | resp[9]);
    } else {
        set_error(error, error_len, "socks5 udp relay atyp not supported");
        return 0;
    }
    snprintf(outbound->udp_relay_host, sizeof(outbound->udp_relay_host), "%s", relay_host);
    outbound->udp_relay_port = relay_port;
    outbound->udp_fd = udp_connect_ipv4(relay_host, relay_port);
    if (outbound->udp_fd < 0) {
        set_error(error, error_len, "socks5 udp relay connect failed");
        return 0;
    }
    stage_log("socks5.udp_associate.done", outbound->udp_relay_host);
    return 1;
}

static int http_connect_tunnel(xgw_outbound_t *outbound, const char *target_host, uint16_t target_port, char *error, size_t error_len) {
    char req[512];
    char resp[512];
    int n;
    stage_log("http.connect.start", outbound->host);
    outbound->tcp_fd = tcp_connect_ipv4(outbound->host, outbound->port);
    if (outbound->tcp_fd < 0) {
        set_error(error, error_len, "http proxy connect failed");
        return 0;
    }
    stage_log("http.connect.proxy_connected", outbound->host);
    snprintf(req,
             sizeof(req),
             "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\nProxy-Connection: Keep-Alive\r\n\r\n",
             target_host,
             target_port,
             target_host,
             target_port);
    if (!tcp_send_all(outbound->tcp_fd, (const uint8_t *) req, strlen(req))) {
        set_error(error, error_len, "http connect request failed");
        return 0;
    }
    stage_log("http.connect.request_sent", target_host);
    n = recv(outbound->tcp_fd, resp, sizeof(resp) - 1, 0);
    if (n <= 0) {
        set_error(error, error_len, "http connect response failed");
        return 0;
    }
    resp[n] = '\0';
    if (strstr(resp, " 200 ") == NULL) {
        set_error(error, error_len, "http connect rejected");
        return 0;
    }
    stage_log("http.connect.done", target_host);
    return 1;
}

int xgw_outbound_parse_type(const char *value, xgw_outbound_type_t *type) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "direct") == 0) {
        *type = XGW_OUTBOUND_DIRECT;
        return 1;
    }
    if (strcmp(value, "fixed") == 0) {
        *type = XGW_OUTBOUND_FIXED;
        return 1;
    }
    if (strcmp(value, "socks5") == 0) {
        *type = XGW_OUTBOUND_SOCKS5;
        return 1;
    }
    if (strcmp(value, "http") == 0) {
        *type = XGW_OUTBOUND_HTTP;
        return 1;
    }
    return 0;
}

const char *xgw_outbound_type_name(xgw_outbound_type_t type) {
    switch (type) {
        case XGW_OUTBOUND_FIXED:
            return "fixed";
        case XGW_OUTBOUND_SOCKS5:
            return "socks5";
        case XGW_OUTBOUND_HTTP:
            return "http";
        case XGW_OUTBOUND_DIRECT:
        default:
            return "direct";
    }
}

int xgw_outbound_open(xgw_outbound_t *outbound,
                      xgw_outbound_type_t type,
                      const char *name,
                      const char *host,
                      uint16_t port,
                      const char *username,
                      const char *password,
                      char *error,
                      size_t error_len) {
    if (!xgw_socket_init()) {
        set_error(error, error_len, "socket init failed");
        return 0;
    }
    memset(outbound, 0, sizeof(*outbound));
    outbound->type = type;
    outbound->tcp_fd = -1;
    outbound->udp_fd = -1;
    snprintf(outbound->name, sizeof(outbound->name), "%s", name == NULL ? "" : name);
    snprintf(outbound->host, sizeof(outbound->host), "%s", host == NULL ? "" : host);
    snprintf(outbound->username, sizeof(outbound->username), "%s", username == NULL ? "" : username);
    snprintf(outbound->password, sizeof(outbound->password), "%s", password == NULL ? "" : password);
    outbound->port = port;
    stage_log("open", xgw_outbound_type_name(type));
    if (type == XGW_OUTBOUND_SOCKS5) {
        if (!socks5_negotiate(outbound, error, error_len)) {
            return 0;
        }
        if (!socks5_udp_associate(outbound, error, error_len)) {
            return 0;
        }
    }
    return 1;
}

int xgw_outbound_connect_tcp(xgw_outbound_t *outbound, const char *target_host, uint16_t target_port, char *error, size_t error_len) {
    if (outbound == NULL) {
        set_error(error, error_len, "outbound is null");
        return 0;
    }
    if (outbound->type == XGW_OUTBOUND_HTTP) {
        stage_log("tcp.http.begin", target_host);
        return http_connect_tunnel(outbound, target_host, target_port, error, error_len);
    }
    if (outbound->type == XGW_OUTBOUND_SOCKS5) {
        uint8_t req[512];
        uint8_t resp[512];
        size_t addr_len;
        stage_log("tcp.socks5.begin", target_host);
        if (!socks5_negotiate(outbound, error, error_len)) {
            return 0;
        }
        req[0] = 0x05;
        req[1] = 0x01;
        req[2] = 0x00;
        build_socks5_addr(target_host, target_port, req + 3, &addr_len);
        if (!tcp_send_all(outbound->tcp_fd, req, 3 + addr_len) || !tcp_recv_exact(outbound->tcp_fd, resp, 4) || resp[1] != 0x00) {
            set_error(error, error_len, "socks5 tcp connect failed");
            return 0;
        }
        stage_log("tcp.socks5.done", target_host);
        return 1;
    }
    stage_log("tcp.direct.begin", target_host);
    outbound->tcp_fd = tcp_connect_ipv4(target_host, target_port);
    if (outbound->tcp_fd < 0) {
        set_error(error, error_len, "tcp connect failed");
        return 0;
    }
    stage_log("tcp.direct.done", target_host);
    return 1;
}

int xgw_outbound_send_tcp(xgw_outbound_t *outbound, const uint8_t *data, size_t data_len) {
    if (outbound == NULL || outbound->tcp_fd < 0) {
        return 0;
    }
    stage_log("tcp.send", outbound->current_target_host);
    return tcp_send_all(outbound->tcp_fd, data, data_len);
}

int xgw_outbound_recv_tcp(xgw_outbound_t *outbound, uint8_t *buf, size_t buf_cap, int timeout_ms) {
    if (outbound == NULL || outbound->tcp_fd < 0) {
        return 0;
    }
    stage_log("tcp.recv.wait", outbound->current_target_host);
    if (!wait_fd_readable(outbound->tcp_fd, timeout_ms)) {
        return 0;
    }
    stage_log("tcp.recv.ready", outbound->current_target_host);
    return recv(outbound->tcp_fd, (char *) buf, (int) buf_cap, 0);
}

int xgw_outbound_send(xgw_outbound_t *outbound, const uint8_t *data, size_t data_len, const char *host, uint16_t port) {
    if (outbound == NULL) {
        return 0;
    }
    if (outbound->type == XGW_OUTBOUND_HTTP) {
        return 0;
    }
    if (outbound->type == XGW_OUTBOUND_SOCKS5) {
        uint8_t pkt[65535];
        size_t addr_len;
        size_t total_len;
        stage_log("udp.socks5.send.begin", host);
        pkt[0] = 0x00;
        pkt[1] = 0x00;
        pkt[2] = 0x00;
        build_socks5_addr(host, port, pkt + 3, &addr_len);
        memcpy(pkt + 3 + addr_len, data, data_len);
        total_len = 3 + addr_len + data_len;
        stage_log("udp.socks5.send.done", host);
        return send(outbound->udp_fd, (const char *) pkt, (int) total_len, 0) == (int) total_len;
    }
    if (outbound->type == XGW_OUTBOUND_FIXED) {
        host = outbound->host;
        port = outbound->port;
    }
    if (outbound->udp_fd < 0 ||
        strcmp(outbound->current_target_host, host == NULL ? "" : host) != 0 ||
        outbound->current_target_port != port) {
        if (outbound->udp_fd >= 0) {
            xgw_close_socket(outbound->udp_fd);
        }
        outbound->udp_fd = udp_connect_ipv4(host, port);
        if (outbound->udp_fd < 0) {
            return 0;
        }
        snprintf(outbound->current_target_host, sizeof(outbound->current_target_host), "%s", host == NULL ? "" : host);
        outbound->current_target_port = port;
    }
    stage_log("udp.direct.send", host);
    return send(outbound->udp_fd, (const char *) data, (int) data_len, 0) == (int) data_len;
}

int xgw_outbound_recv(xgw_outbound_t *outbound, xgw_packet_t *packet, int timeout_ms) {
    fd_set rfds;
    struct timeval tv;
    int ready;
    if (outbound == NULL || packet == NULL || outbound->udp_fd < 0) {
        return 0;
    }
    stage_log("udp.recv.wait", outbound->current_target_host);
    FD_ZERO(&rfds);
    FD_SET((unsigned int) outbound->udp_fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ready = select(outbound->udp_fd + 1, &rfds, NULL, NULL, timeout_ms >= 0 ? &tv : NULL);
    if (ready <= 0) {
        return 0;
    }
    stage_log("udp.recv.ready", outbound->current_target_host);
    packet->data_len = recv(outbound->udp_fd, (char *) packet->data, (int) sizeof(packet->data), 0);
    if ((int) packet->data_len <= 0) {
        return 0;
    }
    if (outbound->type == XGW_OUTBOUND_SOCKS5) {
        size_t hdr = 0;
        if (packet->data_len < 10) {
            return 0;
        }
        hdr = 4;
        if (packet->data[3] == 0x01) {
            hdr += 4 + 2;
        } else if (packet->data[3] == 0x03) {
            hdr += 1 + packet->data[4] + 2;
        } else {
            return 0;
        }
        if (hdr >= packet->data_len) {
            return 0;
        }
        memmove(packet->data, packet->data + hdr, packet->data_len - hdr);
        packet->data_len -= hdr;
    }
    snprintf(packet->remote_host, sizeof(packet->remote_host), "%s", outbound->current_target_host);
    packet->remote_port = outbound->current_target_port;
    return 1;
}

void xgw_outbound_close(xgw_outbound_t *outbound) {
    if (outbound == NULL) {
        return;
    }
    if (outbound->tcp_fd >= 0) {
        xgw_close_socket(outbound->tcp_fd);
    }
    if (outbound->udp_fd >= 0) {
        xgw_close_socket(outbound->udp_fd);
    }
    memset(outbound, 0, sizeof(*outbound));
    outbound->tcp_fd = -1;
    outbound->udp_fd = -1;
}
