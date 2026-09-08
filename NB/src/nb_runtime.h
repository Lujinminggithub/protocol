#ifndef NB_RUNTIME_H
#define NB_RUNTIME_H

#include <picoquic.h>
#include <sys/socket.h>

#include "nb_bridge.h"
#include "nb_pool.h"
#include "nb_path.h"
#include "nb_routes.h"
#include "nb_session.h"

#define NB_MAX_CONNECTIONS 1024
#define NB_MAX_PATH_POOLS 8

typedef struct nb_global {
    nb_role_t role;
    picoquic_quic_t* quic;
    int udp_fd;                          /* QUIC UDP socket */
    int client_udp_fd;                   /* middle 下游独立端口，隔离 worker QUIC context */
    int epoll_fd;
    int control_fd;
    struct sockaddr_storage local_addr;  /* incoming_packet 的 addr_to */
    struct sockaddr_storage client_local_addr;
    struct sockaddr_storage outbound_addr; /* exit 目标 TCP 的指定源地址 */
    int outbound_configured;
    /* entry: 本地 TCP listen */
    int tcp_listen_fd;
    char route_str[300];                 /* entry: 发给第一跳的 route(固定 target 模式) */
    int socks_enabled;                   /* entry: SOCKS5 入口(动态 target) */
    char mid_route[256];                 /* entry SOCKS 模式: 中间跳前缀 "H:kz:4443"(可空=两跳) */
    nb_routes_t exit_routes;
    int exit_routes_enabled;
    int signal_direct_enabled;           /* selected low-volume TCP rules connect directly to exit */
    /* entry/middle: 到下一跳的 QUIC 连接池(每条独立 cwnd/pacing/flow-control, round-robin 分流) */
    cnx_pool_t pools[NB_MAX_PATH_POOLS];
    int pool_count;
    struct sockaddr_storage next_addr;    /* entry: 第一跳地址(启动配, pool_init 用) */
    int next_configured;                  /* entry: next_addr 已配 */
    uint32_t next_ps_id;                  /* proxy_stream 流水号分配 */
    proxy_stream_t streams[NB_MAX_CONNECTIONS];
} nb_global_t;

#endif
