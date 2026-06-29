/* 本地 TCP bridge：把 HY2 前端的 TCP 流量翻译成 xgw 链路可承载的最小载荷。 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define _POSIX_C_SOURCE 200112L

#include "xgw_bridge.h"
#include "xgw_local_adapter.h"

#include "log4c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef int socklen_arg_t;
#define xgw_close_socket closesocket
#define xgw_shutdown_write(fd) shutdown((fd), SD_SEND)
#define XGW_INVALID_SOCKET (-1)
static int xgw_socket_runtime_init(void) {
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
static int xgw_set_nonblocking(int fd) {
    u_long mode = 1UL;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
}
static void xgw_bridge_sleep_ms(unsigned int ms) { Sleep(ms); }
static void xgw_set_tcp_nodelay(int fd) {
    BOOL on = TRUE;
    (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &on, sizeof(on));
}
static void ring_notify_wake(uint32_t *addr) {
    (void) addr;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#ifdef __linux__
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#include <time.h>
#include <unistd.h>
typedef socklen_t socklen_arg_t;
#define SOCKET_ERROR (-1)
#define xgw_close_socket close
#define xgw_shutdown_write(fd) shutdown((fd), SHUT_WR)
#define XGW_INVALID_SOCKET (-1)
static int xgw_socket_runtime_init(void) { return 1; }
static int xgw_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
static void xgw_bridge_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (long) (ms % 1000U) * 1000000L;
    nanosleep(&ts, NULL);
}
static void xgw_set_tcp_nodelay(int fd) {
    int on = 1;
    (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &on, sizeof(on));
}
static void ring_notify_wake(uint32_t *addr) {
#ifdef __linux__
    if (addr != NULL) {
        (void) syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    }
#else
    (void) addr;
#endif
}
#endif

#define XGW_BRIDGE_TRANSPORT_TCP 1
#define XGW_BRIDGE_TRANSPORT_UNIX 2
#define XGW_BRIDGE_TRANSPORT_RING 3
#define XGW_BRIDGE_KIND_TCP_OPEN XGW_LOCAL_ADAPTER_KIND_TCP_OPEN
#define XGW_BRIDGE_KIND_TCP_DATA XGW_LOCAL_ADAPTER_KIND_TCP_DATA
#define XGW_BRIDGE_KIND_TCP_CLOSE XGW_LOCAL_ADAPTER_KIND_TCP_CLOSE
#define XGW_BRIDGE_KIND_TCP_HALF_CLOSE XGW_LOCAL_ADAPTER_KIND_TCP_HALF_CLOSE
#define XGW_BRIDGE_KIND_UDP_OPEN XGW_LOCAL_ADAPTER_KIND_UDP_OPEN
#define XGW_BRIDGE_KIND_UDP_DATA XGW_LOCAL_ADAPTER_KIND_UDP_DATA
#define XGW_BRIDGE_KIND_UDP_CLOSE XGW_LOCAL_ADAPTER_KIND_UDP_CLOSE
#define XGW_BRIDGE_MAGIC XGW_LOCAL_ADAPTER_MAGIC
#define XGW_BRIDGE_MAX_CLIENTS 2048
#define XGW_BRIDGE_MAX_EGRESS_SESSIONS 1024
#define XGW_BRIDGE_MAX_PAYLOAD 16384U
#define XGW_BRIDGE_UDP_RX_BUFFER 65535U
#define XGW_BRIDGE_PENDING_CHUNKS 32U
#define XGW_BRIDGE_PENDING_PAYLOADS 64U
/* handle_ring_frame 三态返回：区分"结构损坏"(致命)与"发送队列暂满"(背压可恢复)。
 * 背压不是错误——TikTok 视频突发时 ring 帧涌入快于发往下一跳的速度，pending 队列
 * 暂满是正常流控。此前两者都返回 0 → poll 返回 0 → 主循环 break → 整个 C 层进程崩溃。
 * 背压时调用方应停止本轮 drain、不前进 read_seq（下轮重读该帧），绝不杀进程。 */
#define XGW_RING_FRAME_OK 1
#define XGW_RING_FRAME_ERR 0
#define XGW_RING_FRAME_BACKPRESSURE (-1)
#define XGW_BRIDGE_WIRE_PAYLOAD_MAX 65535U
#define XGW_BRIDGE_RING_MAGIC 0x58475231U
#define XGW_BRIDGE_RING_CAPACITY 1024U
#define XGW_BRIDGE_RING_SLOT_SIZE 65536U
#define XGW_BRIDGE_RING_VERSION 2U
#define XGW_BRIDGE_RING_HEADER_SIZE 64U
#define XGW_BRIDGE_RING_SLOT_HEAD 16U
#define XGW_BRIDGE_RING_FRAME_MAX (XGW_BRIDGE_RING_SLOT_SIZE - XGW_BRIDGE_RING_SLOT_HEAD)
#define XGW_BRIDGE_RING_NOTIFY_OFFSET 16U
#define XGW_BRIDGE_RING_FLAGS_OFFSET 20U
#define XGW_BRIDGE_RING_GENERATION_OFFSET 24U
#define XGW_BRIDGE_RING_WRITE_SEQ_OFFSET 32U
#define XGW_BRIDGE_RING_READ_SEQ_OFFSET 40U
#define XGW_BRIDGE_RING_WRITER_ROLE_OFFSET 48U
#define XGW_BRIDGE_RING_READER_ROLE_OFFSET 52U
#define XGW_BRIDGE_RING_WRITER_ATTACH_OFFSET 56U
#define XGW_BRIDGE_RING_READER_ATTACH_OFFSET 60U
#define XGW_BRIDGE_RING_BATCH_LIMIT 64U
#define XGW_BRIDGE_RING_KIND_STATUS 1U
#define XGW_BRIDGE_RING_KIND_TCP_OPEN 2U
#define XGW_BRIDGE_RING_KIND_UDP_OPEN 3U
#define XGW_BRIDGE_RING_KIND_TCP_DATA 4U
#define XGW_BRIDGE_RING_KIND_UDP_DATA 5U
#define XGW_BRIDGE_RING_KIND_CLOSE 6U
#define XGW_BRIDGE_RING_KIND_HALF_CLOSE 7U
#define XGW_BRIDGE_PRIORITY_HIGH 2U
#define XGW_BRIDGE_PRIORITY_NORMAL 1U
#define XGW_BRIDGE_PRIORITY_LOW 0U
#define XGW_BRIDGE_META_LEN XGW_LOCAL_ADAPTER_META_LEN
#define XGW_BRIDGE_META_LEN_V1 XGW_LOCAL_ADAPTER_META_LEN_V1
#define XGW_BRIDGE_SESSION_META_LEN XGW_LOCAL_ADAPTER_SESSION_META_LEN
#define XGW_BRIDGE_BUDGET_REDUCED_FEC 0x01U
#define XGW_BRIDGE_BUDGET_FAST_ACK 0x02U
#define XGW_BRIDGE_BUDGET_INDEPENDENT_IO 0x04U
#define XGW_BRIDGE_RING_FLAG_OWNER_READY 0x01U
#define XGW_BRIDGE_RING_FLAG_WRITER_READY 0x02U
#define XGW_BRIDGE_RING_FLAG_READER_READY 0x04U
#define XGW_BRIDGE_RING_ROLE_FRONT 1U
#define XGW_BRIDGE_RING_ROLE_INGRESS 2U
#define XGW_BRIDGE_DNS_CACHE_SIZE 256U
#define XGW_BRIDGE_DNS_CACHE_TTL_US (30ULL * 60ULL * 1000000ULL)
#define XGW_BRIDGE_TCP_CONNECT_TIMEOUT_MS 1500
#define XGW_BRIDGE_UDP_CONNECT_TIMEOUT_MS 500

/* bridge 日志级别：0=安静，1=正常（默认，抑制逐包 deliver/send），2+=逐包 verbose。
 * 由 runtime 从 config.tuning.log_level 设置，避免逐包日志写满磁盘。 */
static uint32_t g_xgw_bridge_log_level = 1U;
void xgw_bridge_set_log_level(uint32_t level) { g_xgw_bridge_log_level = level; }
#define XGW_BRIDGE_VERBOSE() (g_xgw_bridge_log_level >= 2U)

/* egress pending 队列软上限（运行时可配，钳到编译期硬上限 XGW_BRIDGE_PENDING_CHUNKS）。
 * 0=用硬上限。由 runtime 从 config.tuning.bridge_egress_pending_chunks 设置。 */
static size_t g_xgw_bridge_pending_chunks_soft = 0U;
void xgw_bridge_set_pending_chunks(uint32_t chunks) {
    if (chunks == 0U || chunks > XGW_BRIDGE_PENDING_CHUNKS) {
        g_xgw_bridge_pending_chunks_soft = XGW_BRIDGE_PENDING_CHUNKS;
    } else {
        g_xgw_bridge_pending_chunks_soft = (size_t) chunks;
    }
}
static size_t xgw_bridge_pending_limit(void) {
    return g_xgw_bridge_pending_chunks_soft == 0U
               ? (size_t) XGW_BRIDGE_PENDING_CHUNKS
               : g_xgw_bridge_pending_chunks_soft;
}

typedef enum xgw_bridge_proto {
    XGW_BRIDGE_PROTO_TCP = 1,
    XGW_BRIDGE_PROTO_UDP = 2
} xgw_bridge_proto_t;

typedef xgw_local_adapter_flow_meta_t xgw_bridge_flow_meta_t;
typedef xgw_local_adapter_session_meta_t xgw_bridge_session_meta_t;

typedef struct xgw_bridge_dns_cache_entry {
    int valid;
    char host[256];
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint64_t expires_us;
    uint64_t last_used_us;
} xgw_bridge_dns_cache_entry_t;

static xgw_bridge_dns_cache_entry_t g_bridge_dns_cache[XGW_BRIDGE_DNS_CACHE_SIZE];
static size_t g_bridge_dns_cache_next = 0U;

typedef struct xgw_bridge_client {
    int active;
    int fd;
    int is_ring;
    int front_eof;
    int close_sent;
    uint32_t ring_client_id;
    uint32_t stream_id;
    xgw_bridge_proto_t proto;
    char target_host[256];
    uint16_t target_port;
    int open_sent;
    uint8_t udp_rx_buf[XGW_BRIDGE_UDP_RX_BUFFER];
    size_t udp_rx_len;
    uint64_t accepted_us;
    uint64_t first_packet_us;
    uint64_t first_byte_us;
    uint64_t last_activity_us;
    uint64_t return_bytes;
    uint64_t return_chunks;
    uint64_t last_return_us;
    uint32_t priority;
    xgw_bridge_flow_meta_t meta;
    xgw_bridge_session_meta_t session_meta;
    char close_reason[64];
} xgw_bridge_client_t;

typedef struct xgw_bridge_pending_payload {
    size_t len;
    uint32_t priority;
    uint8_t data[XGW_BRIDGE_WIRE_PAYLOAD_MAX];
} xgw_bridge_pending_payload_t;

typedef struct xgw_bridge_ring_file {
    FILE *fp;
    char path[160];
    uint8_t *map;
    size_t map_len;
#ifdef _WIN32
    HANDLE map_handle;
#endif
    uint32_t capacity;
    uint32_t slot_size;
    uint64_t generation;
    uint64_t attached_generation;
    uint32_t flags;
    uint32_t writer_role;
    uint32_t reader_role;
    uint32_t local_ready_flag;
    uint32_t peer_ready_flag;
    size_t local_attach_offset;
    size_t peer_attach_offset;
} xgw_bridge_ring_file_t;

struct xgw_bridge_server {
    int listen_fd;
    int listen_transport;
    uint32_t next_stream_id;
    xgw_runtime_config_t config;
    xgw_bridge_client_t clients[XGW_BRIDGE_MAX_CLIENTS];
    xgw_bridge_pending_payload_t pending[XGW_BRIDGE_PENDING_PAYLOADS];
    size_t pending_head;
    size_t pending_count;
    xgw_bridge_ring_file_t ring_rx;
    xgw_bridge_ring_file_t ring_tx;
};

typedef struct xgw_bridge_egress_session {
    int active;
    int fd;
    uint32_t stream_id;
    char target_host[256];
    uint16_t target_port;
    char upstream_host[64];
    uint16_t upstream_port;
    int pending_close;
    int is_udp;
    uint64_t open_started_us;
    uint64_t connected_us;
    uint64_t first_send_us;
    uint64_t first_recv_us;
    uint64_t last_activity_us;
    uint64_t send_bytes_total;
    uint64_t send_chunks_total;
    uint64_t recv_bytes_total;
    uint64_t recv_chunks_total;
    uint64_t last_send_log_us;
    uint64_t last_recv_log_us;
    uint32_t priority;
    xgw_bridge_flow_meta_t meta;
    xgw_bridge_session_meta_t session_meta;
    char close_reason[64];
    uint8_t pending_data[XGW_BRIDGE_PENDING_CHUNKS][XGW_BRIDGE_MAX_PAYLOAD];
    size_t pending_lengths[XGW_BRIDGE_PENDING_CHUNKS];
    char pending_addr[XGW_BRIDGE_PENDING_CHUNKS][256];
    size_t pending_head;
    size_t pending_count;
} xgw_bridge_egress_session_t;

struct xgw_bridge_egress {
    xgw_bridge_egress_session_t sessions[XGW_BRIDGE_MAX_EGRESS_SESSIONS];
    char upstream_host[64];
    uint16_t upstream_port;
    char last_upstream_host[64];
    uint16_t last_upstream_port;
    /* burst 出队的 round-robin 游标，保证同优先级流公平、不饿死。 */
    size_t rr_cursor;
};

static void set_error(char *error, size_t error_len, const char *text) {
    if (error_len > 0U) {
        snprintf(error, error_len, "%s", text == NULL ? "" : text);
    }
}

static uint16_t socket_local_port(int fd);
static uint16_t read_be16(const uint8_t *p);
static void decode_bridge_meta_compat(const uint8_t *src, size_t src_len, xgw_bridge_flow_meta_t *meta);
static void decode_bridge_session_meta_compat(const uint8_t *src, size_t src_len, xgw_bridge_session_meta_t *meta);
static int bridge_wait_connect_ready(int fd, int timeout_ms);
static int bridge_resolve_endpoint_cached(const char *host, uint16_t port,
                                          struct sockaddr_storage *addr, socklen_t *addr_len);

static uint64_t xgw_bridge_now_us(void) {
#ifdef _WIN32
    return (uint64_t) GetTickCount64() * 1000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000ULL) + ((uint64_t) ts.tv_nsec / 1000ULL);
#endif
}

static unsigned long long xgw_bridge_elapsed_ms(uint64_t start_us) {
    uint64_t now;
    if (start_us == 0U) {
        return 0ULL;
    }
    now = xgw_bridge_now_us();
    if (now <= start_us) {
        return 0ULL;
    }
    return (unsigned long long) ((now - start_us) / 1000ULL);
}

static const char *bridge_proto_name(xgw_bridge_proto_t proto) {
    return proto == XGW_BRIDGE_PROTO_UDP ? "udp" : "tcp";
}

static const char *bridge_transport_name(const xgw_bridge_client_t *client) {
    if (client == NULL) {
        return "socket";
    }
    return client->is_ring ? "shared-ring" : "socket";
}

static int contains_ignore_case_bridge(const char *text, const char *needle) {
    size_t i;
    size_t needle_len;
    if (text == NULL || needle == NULL) {
        return 0;
    }
    needle_len = strlen(needle);
    if (needle_len == 0U) {
        return 0;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        size_t j;
        for (j = 0U; j < needle_len; ++j) {
            char a = text[i + j];
            char b = needle[j];
            if (a == '\0') {
                return 0;
            }
            if (a >= 'A' && a <= 'Z') {
                a = (char) (a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char) (b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (j == needle_len) {
            return 1;
        }
    }
    return 0;
}

static int is_tiktok_trace_target(const char *host) {
    if (host == NULL || host[0] == '\0') {
        return 0;
    }
    return contains_ignore_case_bridge(host, "mon-boot.tiktokv.com") ||
           contains_ignore_case_bridge(host, "api-boot.tiktokv.com") ||
           contains_ignore_case_bridge(host, "tnc-boot.tiktokv.com") ||
           contains_ignore_case_bridge(host, "frontier.tiktokv.com") ||
           contains_ignore_case_bridge(host, "log-boot.tiktokv.com") ||
           contains_ignore_case_bridge(host, "gecko-boot.tiktokv.com") ||
           contains_ignore_case_bridge(host, "jsb-boot.tiktokv.com");
}

static uint32_t bridge_classify_priority(const char *host, uint16_t port) {
    if (host == NULL || host[0] == '\0') {
        return XGW_BRIDGE_PRIORITY_NORMAL;
    }
    if (contains_ignore_case_bridge(host, "google.com") ||
        contains_ignore_case_bridge(host, "ip.sb") ||
        contains_ignore_case_bridge(host, "ocsp2.apple.com") ||
        contains_ignore_case_bridge(host, "updates.cdn-apple.com")) {
        return XGW_BRIDGE_PRIORITY_HIGH;
    }
    if (port == 5223U && contains_ignore_case_bridge(host, "courier.push.apple.com")) {
        return XGW_BRIDGE_PRIORITY_HIGH;
    }
    if (contains_ignore_case_bridge(host, "api-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "api-core-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "mon-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "tnc-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "mssdk-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "jsb-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "frontier.tiktokv.com") ||
        contains_ignore_case_bridge(host, "log-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "webcast-boot.tiktokv.com") ||
        contains_ignore_case_bridge(host, "inapp.tiktokv.com") ||
        contains_ignore_case_bridge(host, "pitayacdn.tiktokcdn.com") ||
        contains_ignore_case_bridge(host, "sf16-website-login.neutral.ttwstatic.com") ||
        contains_ignore_case_bridge(host, "api16-normal-")) {
        return XGW_BRIDGE_PRIORITY_HIGH;
    }
    if (contains_ignore_case_bridge(host, "tiktok") ||
        contains_ignore_case_bridge(host, "tiktokcdn") ||
        contains_ignore_case_bridge(host, "ttwstatic") ||
        contains_ignore_case_bridge(host, "pitaya-clientai")) {
        return XGW_BRIDGE_PRIORITY_NORMAL;
    }
    return XGW_BRIDGE_PRIORITY_LOW;
}

static void bridge_client_set_close_reason(xgw_bridge_client_t *client, const char *reason) {
    if (client == NULL || reason == NULL || reason[0] == '\0') {
        return;
    }
    if (client->close_reason[0] == '\0') {
        snprintf(client->close_reason, sizeof(client->close_reason), "%s", reason);
    }
}

static void bridge_default_meta(xgw_bridge_flow_meta_t *meta, uint32_t priority) {
    if (meta == NULL) {
        return;
    }
    memset(meta, 0, sizeof(*meta));
    meta->priority = (uint8_t) priority;
    meta->preferred_copies = 1U;
    if (priority >= XGW_BRIDGE_PRIORITY_HIGH) {
        meta->budget_flags = XGW_BRIDGE_BUDGET_REDUCED_FEC | XGW_BRIDGE_BUDGET_FAST_ACK;
        meta->read_timeout_ms = 180000U;
        meta->idle_after_first_byte_ms = 180000U;
    } else if (priority == XGW_BRIDGE_PRIORITY_NORMAL) {
        meta->read_timeout_ms = 180000U;
        meta->idle_after_first_byte_ms = 180000U;
    } else {
        meta->read_timeout_ms = 180000U;
        meta->idle_after_first_byte_ms = 180000U;
    }
}

static void bridge_default_session_meta(xgw_bridge_session_meta_t *meta) {
    if (meta == NULL) {
        return;
    }
    memset(meta, 0, sizeof(*meta));
}

static void bridge_apply_meta_defaults(xgw_bridge_client_t *client) {
    if (client == NULL) {
        return;
    }
    bridge_default_meta(&client->meta, client->priority);
}

static void bridge_client_touch_activity(xgw_bridge_client_t *client) {
    if (client == NULL || !client->active) {
        return;
    }
    client->last_activity_us = xgw_bridge_now_us();
}

static void bridge_client_log_accept(const xgw_bridge_client_t *client) {
    if (client == NULL) {
        return;
    }
    printf("ingress.phase.accept stream=%u proto=%s target=%s:%u priority=%u transport=%s frontend=%s front_session_id=%s route=%s line=%s\n",
           client->stream_id,
           bridge_proto_name(client->proto),
           client->target_host,
           client->target_port,
           client->priority,
           bridge_transport_name(client),
           client->session_meta.frontend,
           client->session_meta.session_id,
           client->session_meta.route_name,
           client->session_meta.line_id);
    fflush(stdout);
    if (is_tiktok_trace_target(client->target_host)) {
        printf("tiktok.trace.ingress.accept stream=%u proto=%s target=%s:%u frontend=%s front_session_id=%s route=%s line=%s\n",
               client->stream_id,
               bridge_proto_name(client->proto),
               client->target_host,
               client->target_port,
               client->session_meta.frontend,
               client->session_meta.session_id,
               client->session_meta.route_name,
               client->session_meta.line_id);
        fflush(stdout);
    }
}

static void bridge_client_note_first_packet(xgw_bridge_client_t *client, size_t bytes, const char *source) {
    uint64_t now_us;
    if (client == NULL || !client->active || bytes == 0U) {
        return;
    }
    now_us = xgw_bridge_now_us();
    client->last_activity_us = now_us;
    if (client->first_packet_us != 0U) {
        return;
    }
    client->first_packet_us = now_us;
    printf("ingress.phase.first_packet stream=%u proto=%s target=%s:%u elapsed_ms=%llu bytes=%zu source=%s transport=%s\n",
           client->stream_id,
           bridge_proto_name(client->proto),
           client->target_host,
           client->target_port,
           xgw_bridge_elapsed_ms(client->accepted_us),
           bytes,
           source == NULL ? "" : source,
           bridge_transport_name(client));
    fflush(stdout);
    if (is_tiktok_trace_target(client->target_host)) {
        printf("tiktok.trace.ingress.first_packet stream=%u proto=%s target=%s:%u elapsed_ms=%llu bytes=%zu source=%s\n",
               client->stream_id,
               bridge_proto_name(client->proto),
               client->target_host,
               client->target_port,
               xgw_bridge_elapsed_ms(client->accepted_us),
               bytes,
               source == NULL ? "" : source);
        fflush(stdout);
    }
}

static void bridge_client_note_first_byte(xgw_bridge_client_t *client, size_t bytes, const char *source) {
    uint64_t now_us;
    if (client == NULL || !client->active || bytes == 0U) {
        return;
    }
    now_us = xgw_bridge_now_us();
    client->last_activity_us = now_us;
    if (client->first_byte_us != 0U) {
        return;
    }
    client->first_byte_us = now_us;
    printf("ingress.phase.first_byte stream=%u proto=%s target=%s:%u elapsed_ms=%llu bytes=%zu source=%s transport=%s\n",
           client->stream_id,
           bridge_proto_name(client->proto),
           client->target_host,
           client->target_port,
           xgw_bridge_elapsed_ms(client->accepted_us),
           bytes,
           source == NULL ? "" : source,
           bridge_transport_name(client));
    fflush(stdout);
    if (is_tiktok_trace_target(client->target_host)) {
        printf("tiktok.trace.ingress.first_byte stream=%u proto=%s target=%s:%u elapsed_ms=%llu bytes=%zu source=%s\n",
               client->stream_id,
               bridge_proto_name(client->proto),
               client->target_host,
               client->target_port,
               xgw_bridge_elapsed_ms(client->accepted_us),
               bytes,
               source == NULL ? "" : source);
        fflush(stdout);
    }
}

static void bridge_client_note_return_chunk(xgw_bridge_client_t *client, size_t bytes, const char *source) {
    uint64_t now_us;
    unsigned long long gap_ms = 0ULL;
    if (client == NULL || !client->active || bytes == 0U) {
        return;
    }
    now_us = xgw_bridge_now_us();
    if (client->last_return_us > 0U && now_us > client->last_return_us) {
        gap_ms = (unsigned long long) ((now_us - client->last_return_us) / 1000ULL);
    }
    client->last_return_us = now_us;
    client->return_bytes += (uint64_t) bytes;
    client->return_chunks += 1U;
    printf("ingress.phase.return_chunk stream=%u proto=%s target=%s:%u chunk_bytes=%zu total_bytes=%llu chunks=%llu gap_ms=%llu source=%s transport=%s\n",
           client->stream_id,
           bridge_proto_name(client->proto),
           client->target_host,
           client->target_port,
           bytes,
           (unsigned long long) client->return_bytes,
           (unsigned long long) client->return_chunks,
           gap_ms,
           source == NULL ? "" : source,
           bridge_transport_name(client));
    fflush(stdout);
}

static void bridge_client_log_close(const xgw_bridge_client_t *client) {
    unsigned long long first_packet_ms = 0ULL;
    unsigned long long first_byte_ms = 0ULL;
    if (client == NULL) {
        return;
    }
    if (client->first_packet_us > client->accepted_us && client->accepted_us > 0U) {
        first_packet_ms = (unsigned long long) ((client->first_packet_us - client->accepted_us) / 1000ULL);
    }
    if (client->first_byte_us > client->accepted_us && client->accepted_us > 0U) {
        first_byte_ms = (unsigned long long) ((client->first_byte_us - client->accepted_us) / 1000ULL);
    }
    printf("ingress.phase.close stream=%u proto=%s target=%s:%u lifetime_ms=%llu first_packet_ms=%llu first_byte_ms=%llu return_bytes=%llu return_chunks=%llu reason=%s transport=%s\n",
           client->stream_id,
           bridge_proto_name(client->proto),
           client->target_host,
           client->target_port,
           xgw_bridge_elapsed_ms(client->accepted_us),
           first_packet_ms,
           first_byte_ms,
           (unsigned long long) client->return_bytes,
           (unsigned long long) client->return_chunks,
           client->close_reason[0] == '\0' ? "closed" : client->close_reason,
           bridge_transport_name(client));
    fflush(stdout);
    if (is_tiktok_trace_target(client->target_host)) {
        printf("tiktok.trace.ingress.close stream=%u proto=%s target=%s:%u lifetime_ms=%llu first_packet_ms=%llu first_byte_ms=%llu return_bytes=%llu reason=%s\n",
               client->stream_id,
               bridge_proto_name(client->proto),
               client->target_host,
               client->target_port,
               xgw_bridge_elapsed_ms(client->accepted_us),
               first_packet_ms,
               first_byte_ms,
               (unsigned long long) client->return_bytes,
               client->close_reason[0] == '\0' ? "closed" : client->close_reason);
        fflush(stdout);
    }
}

static void egress_session_set_close_reason(xgw_bridge_egress_session_t *session, const char *reason) {
    if (session == NULL || reason == NULL || reason[0] == '\0') {
        return;
    }
    if (session->close_reason[0] == '\0') {
        snprintf(session->close_reason, sizeof(session->close_reason), "%s", reason);
    }
}

static void egress_session_log_open(const xgw_bridge_egress_session_t *session) {
    if (session == NULL) {
        return;
    }
    printf("egress.phase.open stream=%u proto=%s target=%s:%u priority=%u connect_ms=%llu upstream=%s:%u local_port=%u\n",
           session->stream_id,
           session->is_udp ? "udp" : "tcp",
           session->target_host,
           session->target_port,
           session->priority,
           session->connected_us > session->open_started_us && session->open_started_us > 0U
               ? (unsigned long long) ((session->connected_us - session->open_started_us) / 1000ULL)
               : 0ULL,
           session->upstream_host,
           session->upstream_port,
           session->fd >= 0 ? socket_local_port(session->fd) : 0U);
    fflush(stdout);
    if (!session->is_udp && is_tiktok_trace_target(session->target_host)) {
        printf("tiktok.trace.egress.open stream=%u target=%s:%u connect_ms=%llu upstream=%s:%u local_port=%u\n",
               session->stream_id,
               session->target_host,
               session->target_port,
               session->connected_us > session->open_started_us && session->open_started_us > 0U
                   ? (unsigned long long) ((session->connected_us - session->open_started_us) / 1000ULL)
                   : 0ULL,
               session->upstream_host,
               session->upstream_port,
               session->fd >= 0 ? socket_local_port(session->fd) : 0U);
        fflush(stdout);
    }
}

static int bridge_wait_connect_ready(int fd, int timeout_ms) {
    fd_set wfds;
    struct timeval tv;
    int ready;
    int so_error = 0;
    socklen_arg_t so_len = (socklen_arg_t) sizeof(so_error);
    if (fd < 0) {
        return 0;
    }
    FD_ZERO(&wfds);
    FD_SET((unsigned int) fd, &wfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ready = select(fd + 1, NULL, &wfds, NULL, timeout_ms >= 0 ? &tv : NULL);
    if (ready <= 0 || !FD_ISSET((unsigned int) fd, &wfds)) {
        return 0;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &so_error, &so_len) != 0) {
        return 0;
    }
    return so_error == 0;
}

static int bridge_resolve_endpoint_cached(const char *host, uint16_t port,
                                          struct sockaddr_storage *addr, socklen_t *addr_len) {
    size_t i;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;
    char port_text[8];
    uint64_t now_us;
    size_t slot = XGW_BRIDGE_DNS_CACHE_SIZE;
    if (host == NULL || host[0] == '\0' || addr == NULL || addr_len == NULL) {
        return 0;
    }
    memset(addr, 0, sizeof(*addr));
    /* IP 字面量（含 IPv6）直接构造，跳过解析与缓存。 */
    {
        struct in_addr v4;
        struct in6_addr v6;
        if (inet_pton(AF_INET, host, &v4) == 1) {
            struct sockaddr_in *sin = (struct sockaddr_in *) addr;
            sin->sin_family = AF_INET;
            sin->sin_port = htons(port);
            sin->sin_addr = v4;
            *addr_len = (socklen_t) sizeof(struct sockaddr_in);
            return 1;
        }
        if (inet_pton(AF_INET6, host, &v6) == 1) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) addr;
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port = htons(port);
            sin6->sin6_addr = v6;
            *addr_len = (socklen_t) sizeof(struct sockaddr_in6);
            return 1;
        }
    }
    now_us = xgw_bridge_now_us();
    /* 缓存命中：缓存里保存的是不带端口的地址，命中后补上当前端口。 */
    for (i = 0U; i < XGW_BRIDGE_DNS_CACHE_SIZE; ++i) {
        if (!g_bridge_dns_cache[i].valid) {
            if (slot == XGW_BRIDGE_DNS_CACHE_SIZE) {
                slot = i;
            }
            continue;
        }
        if (strcmp(g_bridge_dns_cache[i].host, host) != 0) {
            if (g_bridge_dns_cache[i].expires_us <= now_us && slot == XGW_BRIDGE_DNS_CACHE_SIZE) {
                slot = i;
            }
            continue;
        }
        if (g_bridge_dns_cache[i].expires_us > now_us) {
            g_bridge_dns_cache[i].last_used_us = now_us;
            memcpy(addr, &g_bridge_dns_cache[i].addr, sizeof(*addr));
            *addr_len = g_bridge_dns_cache[i].addr_len;
            if (addr->ss_family == AF_INET) {
                ((struct sockaddr_in *) addr)->sin_port = htons(port);
            } else if (addr->ss_family == AF_INET6) {
                ((struct sockaddr_in6 *) addr)->sin6_port = htons(port);
            }
            return 1;
        }
        slot = i;
        break;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* 同时解析 A 与 AAAA */
    hints.ai_socktype = SOCK_STREAM;
#ifdef AI_ADDRCONFIG
    hints.ai_flags = AI_ADDRCONFIG;
#endif
    snprintf(port_text, sizeof(port_text), "%u", (unsigned int) port);
    if (getaddrinfo(host, port_text, &hints, &result) != 0 || result == NULL) {
        return 0;
    }
    /* 优先 IPv4（与历史行为一致、最稳），无 A 记录时回退首个 AAAA。 */
    it = NULL;
    {
        struct addrinfo *first = NULL;
        struct addrinfo *cur;
        for (cur = result; cur != NULL; cur = cur->ai_next) {
            if (cur->ai_family != AF_INET && cur->ai_family != AF_INET6) {
                continue;
            }
            if (first == NULL) {
                first = cur;
            }
            if (cur->ai_family == AF_INET) {
                it = cur;
                break;
            }
        }
        if (it == NULL) {
            it = first;
        }
    }
    if (it == NULL || it->ai_addrlen > (socklen_arg_t) sizeof(*addr)) {
        freeaddrinfo(result);
        return 0;
    }
    memcpy(addr, it->ai_addr, it->ai_addrlen);
    *addr_len = (socklen_t) it->ai_addrlen;
    freeaddrinfo(result);
    if (slot == XGW_BRIDGE_DNS_CACHE_SIZE) {
        slot = g_bridge_dns_cache_next++ % XGW_BRIDGE_DNS_CACHE_SIZE;
    }
    memset(&g_bridge_dns_cache[slot], 0, sizeof(g_bridge_dns_cache[slot]));
    g_bridge_dns_cache[slot].valid = 1;
    snprintf(g_bridge_dns_cache[slot].host, sizeof(g_bridge_dns_cache[slot].host), "%s", host);
    memcpy(&g_bridge_dns_cache[slot].addr, addr, sizeof(g_bridge_dns_cache[slot].addr));
    g_bridge_dns_cache[slot].addr_len = *addr_len;
    g_bridge_dns_cache[slot].last_used_us = now_us;
    g_bridge_dns_cache[slot].expires_us = now_us + XGW_BRIDGE_DNS_CACHE_TTL_US;
    return 1;
}

static void egress_session_touch_activity(xgw_bridge_egress_session_t *session) {
    if (session == NULL || !session->active) {
        return;
    }
    session->last_activity_us = xgw_bridge_now_us();
}

static void egress_session_note_first_send(xgw_bridge_egress_session_t *session, size_t bytes) {
    uint64_t now_us;
    if (session == NULL || !session->active || bytes == 0U) {
        return;
    }
    now_us = xgw_bridge_now_us();
    session->last_activity_us = now_us;
    if (session->first_send_us != 0U) {
        return;
    }
    session->first_send_us = now_us;
    printf("egress.phase.first_packet stream=%u proto=%s target=%s:%u elapsed_ms=%llu bytes=%zu\n",
           session->stream_id,
           session->is_udp ? "udp" : "tcp",
           session->target_host,
           session->target_port,
           xgw_bridge_elapsed_ms(session->open_started_us),
           bytes);
    fflush(stdout);
    if (!session->is_udp && is_tiktok_trace_target(session->target_host)) {
        printf("tiktok.trace.egress.first_packet stream=%u target=%s:%u elapsed_ms=%llu bytes=%zu\n",
               session->stream_id,
               session->target_host,
               session->target_port,
               xgw_bridge_elapsed_ms(session->open_started_us),
               bytes);
        fflush(stdout);
    }
}

static void egress_session_note_first_recv(xgw_bridge_egress_session_t *session, size_t bytes) {
    uint64_t now_us;
    if (session == NULL || !session->active || bytes == 0U) {
        return;
    }
    now_us = xgw_bridge_now_us();
    session->last_activity_us = now_us;
    if (session->first_recv_us != 0U) {
        return;
    }
    session->first_recv_us = now_us;
    /* 回程时间线埋点(带绝对时间戳):egress 从上游收到首字节的时刻。
     * 与 runtime 的"帧发往 relay"、ingress 的"收到回程帧"对比,定位 3.3s 花在哪一段。 */
    log4c_info("rtt.egress.upstream_first_byte stream=%u target=%s:%u bytes=%zu",
               session->stream_id, session->target_host, session->target_port, bytes);
    printf("egress.phase.first_byte stream=%u proto=%s target=%s:%u elapsed_ms=%llu bytes=%zu\n",
           session->stream_id,
           session->is_udp ? "udp" : "tcp",
           session->target_host,
           session->target_port,
           xgw_bridge_elapsed_ms(session->open_started_us),
           bytes);
    fflush(stdout);
    if (!session->is_udp && is_tiktok_trace_target(session->target_host)) {
        printf("tiktok.trace.egress.first_byte stream=%u target=%s:%u elapsed_ms=%llu bytes=%zu\n",
               session->stream_id,
               session->target_host,
               session->target_port,
               xgw_bridge_elapsed_ms(session->open_started_us),
               bytes);
        fflush(stdout);
    }
}

static void egress_session_note_send_chunk(xgw_bridge_egress_session_t *session, size_t bytes) {
    uint64_t now_us;
    unsigned long long gap_ms = 0ULL;
    if (session == NULL || !session->active || bytes == 0U) {
        return;
    }
    now_us = xgw_bridge_now_us();
    if (session->last_send_log_us > 0U && now_us > session->last_send_log_us) {
        gap_ms = (unsigned long long) ((now_us - session->last_send_log_us) / 1000ULL);
    }
    session->last_send_log_us = now_us;
    session->send_bytes_total += (uint64_t) bytes;
    session->send_chunks_total += 1U;
    printf("egress.phase.send_chunk stream=%u proto=%s target=%s:%u chunk_bytes=%zu total_bytes=%llu chunks=%llu gap_ms=%llu\n",
           session->stream_id,
           session->is_udp ? "udp" : "tcp",
           session->target_host,
           session->target_port,
           bytes,
           (unsigned long long) session->send_bytes_total,
           (unsigned long long) session->send_chunks_total,
           gap_ms);
    fflush(stdout);
}

static void egress_session_note_recv_chunk(xgw_bridge_egress_session_t *session, size_t bytes) {
    uint64_t now_us;
    unsigned long long gap_ms = 0ULL;
    if (session == NULL || !session->active || bytes == 0U) {
        return;
    }
    now_us = xgw_bridge_now_us();
    if (session->last_recv_log_us > 0U && now_us > session->last_recv_log_us) {
        gap_ms = (unsigned long long) ((now_us - session->last_recv_log_us) / 1000ULL);
    }
    session->last_recv_log_us = now_us;
    session->recv_bytes_total += (uint64_t) bytes;
    session->recv_chunks_total += 1U;
    printf("egress.phase.recv_chunk stream=%u proto=%s target=%s:%u chunk_bytes=%zu total_bytes=%llu chunks=%llu gap_ms=%llu\n",
           session->stream_id,
           session->is_udp ? "udp" : "tcp",
           session->target_host,
           session->target_port,
           bytes,
           (unsigned long long) session->recv_bytes_total,
           (unsigned long long) session->recv_chunks_total,
           gap_ms);
    fflush(stdout);
}

static void egress_session_log_close(const xgw_bridge_egress_session_t *session) {
    unsigned long long first_packet_ms = 0ULL;
    unsigned long long first_byte_ms = 0ULL;
    if (session == NULL) {
        return;
    }
    if (session->first_send_us > session->open_started_us && session->open_started_us > 0U) {
        first_packet_ms = (unsigned long long) ((session->first_send_us - session->open_started_us) / 1000ULL);
    }
    if (session->first_recv_us > session->open_started_us && session->open_started_us > 0U) {
        first_byte_ms = (unsigned long long) ((session->first_recv_us - session->open_started_us) / 1000ULL);
    }
    printf("egress.phase.close stream=%u proto=%s target=%s:%u lifetime_ms=%llu first_packet_ms=%llu first_byte_ms=%llu send_bytes=%llu send_chunks=%llu recv_bytes=%llu recv_chunks=%llu reason=%s upstream=%s:%u\n",
           session->stream_id,
           session->is_udp ? "udp" : "tcp",
           session->target_host,
           session->target_port,
           xgw_bridge_elapsed_ms(session->open_started_us),
           first_packet_ms,
           first_byte_ms,
           (unsigned long long) session->send_bytes_total,
           (unsigned long long) session->send_chunks_total,
           (unsigned long long) session->recv_bytes_total,
           (unsigned long long) session->recv_chunks_total,
           session->close_reason[0] == '\0' ? "closed" : session->close_reason,
           session->upstream_host,
           session->upstream_port);
    fflush(stdout);
    if (!session->is_udp && is_tiktok_trace_target(session->target_host)) {
        printf("tiktok.trace.egress.close stream=%u target=%s:%u lifetime_ms=%llu first_packet_ms=%llu first_byte_ms=%llu send_bytes=%llu recv_bytes=%llu reason=%s upstream=%s:%u\n",
               session->stream_id,
               session->target_host,
               session->target_port,
               xgw_bridge_elapsed_ms(session->open_started_us),
               first_packet_ms,
               first_byte_ms,
               (unsigned long long) session->send_bytes_total,
               (unsigned long long) session->recv_bytes_total,
               session->close_reason[0] == '\0' ? "closed" : session->close_reason,
               session->upstream_host,
               session->upstream_port);
        fflush(stdout);
    }
}

static int parse_host_port_text(const char *text, char *host, size_t host_len, uint16_t *port) {
    const char *colon = strrchr(text, ':');
    char *end = NULL;
    unsigned long parsed;
    if (colon == NULL || colon == text) {
        return 0;
    }
    snprintf(host, host_len, "%.*s", (int) (colon - text), text);
    parsed = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || parsed > 65535UL) {
        return 0;
    }
    *port = (uint16_t) parsed;
    return 1;
}

static int send_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0U;
    while (sent < len) {
        int rc = send(fd, (const char *) (buf + sent), (int) (len - sent), 0);
        if (rc <= 0) {
            return 0;
        }
        sent += (size_t) rc;
    }
    return 1;
}

static int recv_all(int fd, uint8_t *buf, size_t len) {
    size_t received = 0U;
    while (received < len) {
        int rc = recv(fd, (char *) (buf + received), (int) (len - received), 0);
        if (rc <= 0) {
            return 0;
        }
        received += (size_t) rc;
    }
    return 1;
}

static void ensure_parent_dir(const char *path) {
    char tmp[256];
    size_t i;
    if (path == NULL || path[0] == '\0') {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1U; tmp[i] != '\0'; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0') {
#ifdef _WIN32
                (void) _mkdir(tmp);
#else
                (void) mkdir(tmp, 0700);
#endif
            }
            tmp[i] = saved;
        }
    }
}

static int recv_optional_meta(int fd, xgw_bridge_flow_meta_t *meta, xgw_bridge_session_meta_t *session_meta) {
    uint8_t meta_buf[XGW_BRIDGE_META_LEN + XGW_BRIDGE_SESSION_META_LEN];
    int attempts;
    bridge_default_meta(meta, XGW_BRIDGE_PRIORITY_NORMAL);
    bridge_default_session_meta(session_meta);
    for (attempts = 0; attempts < 3; ++attempts) {
        fd_set rfds;
        struct timeval tv;
        int ready;
        FD_ZERO(&rfds);
        FD_SET((unsigned int) fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = attempts == 0 ? 0 : 1000;
        ready = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0 || !FD_ISSET((unsigned int) fd, &rfds)) {
            return 1;
        }
        ready = recv(fd, (char *) meta_buf, (int) sizeof(meta_buf), MSG_PEEK);
        if (ready >= (int) XGW_BRIDGE_META_LEN_V1) {
            size_t want = (size_t) ready;
            if (want > sizeof(meta_buf)) {
                want = sizeof(meta_buf);
            }
            if (!recv_all(fd, meta_buf, want)) {
                return 0;
            }
            if (meta != NULL) {
                meta->flow_class = meta_buf[0];
                meta->priority = meta_buf[1];
                meta->budget_flags = meta_buf[2];
                meta->preferred_copies = meta_buf[3] == 0U ? 1U : meta_buf[3];
                decode_bridge_meta_compat(meta_buf, want >= XGW_BRIDGE_META_LEN ? XGW_BRIDGE_META_LEN : XGW_BRIDGE_META_LEN_V1, meta);
            }
            if (session_meta != NULL && want > XGW_BRIDGE_META_LEN) {
                decode_bridge_session_meta_compat(meta_buf + XGW_BRIDGE_META_LEN, want - XGW_BRIDGE_META_LEN, session_meta);
            }
            return 1;
        }
        if (ready <= 0) {
            return 1;
        }
    }
    return 1;
}

static int recv_request_ex(int fd,
                           xgw_bridge_proto_t *proto,
                           char *host,
                           size_t host_cap,
                           uint16_t *port,
                           xgw_bridge_flow_meta_t *meta,
                           xgw_bridge_session_meta_t *session_meta) {
    uint16_t host_len = 0U;
    uint8_t len_buf[2];
    uint8_t port_buf[2];
    if (!recv_all(fd, len_buf, sizeof(len_buf))) {
        return 0;
    }
    host_len = (uint16_t) (((uint16_t) len_buf[0] << 8U) | (uint16_t) len_buf[1]);
    if (host_len == 0U) {
        uint8_t proto_buf[1];
        uint8_t host_len_buf[2];
        if (!recv_all(fd, proto_buf, sizeof(proto_buf)) || proto_buf[0] != XGW_BRIDGE_PROTO_UDP) {
            return 0;
        }
        *proto = XGW_BRIDGE_PROTO_UDP;
        if (!recv_all(fd, host_len_buf, sizeof(host_len_buf))) {
            return 0;
        }
        host_len = (uint16_t) (((uint16_t) host_len_buf[0] << 8U) | (uint16_t) host_len_buf[1]);
    } else {
        *proto = XGW_BRIDGE_PROTO_TCP;
    }
    if (host_len == 0U || host_len >= host_cap) {
        return 0;
    }
    if (!recv_all(fd, (uint8_t *) host, host_len)) {
        return 0;
    }
    host[host_len] = '\0';
    if (!recv_all(fd, port_buf, sizeof(port_buf))) {
        return 0;
    }
    *port = (uint16_t) (((uint16_t) port_buf[0] << 8U) | (uint16_t) port_buf[1]);
    return recv_optional_meta(fd, meta, session_meta);
}

static int tcp_connect_ipv4(const char *host, uint16_t port) {
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    int fd = -1;
    if (!bridge_resolve_endpoint_cached(host, port, &addr, &addr_len)) {
        return -1;
    }
    fd = (int) socket(addr.ss_family, SOCK_STREAM, 0);
    if (fd == XGW_INVALID_SOCKET) {
        return -1;
    }
    if (!xgw_set_nonblocking(fd)) {
        xgw_close_socket(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *) &addr, (socklen_arg_t) addr_len) == 0) {
        xgw_set_tcp_nodelay(fd);
        return fd;
    }
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK &&
        WSAGetLastError() != WSAEINPROGRESS &&
        WSAGetLastError() != WSAEINVAL) {
        xgw_close_socket(fd);
        return -1;
    }
#else
    if (errno != EINPROGRESS && errno != EALREADY) {
        xgw_close_socket(fd);
        return -1;
    }
#endif
    if (!bridge_wait_connect_ready(fd, XGW_BRIDGE_TCP_CONNECT_TIMEOUT_MS)) {
        xgw_close_socket(fd);
        return -1;
    }
    xgw_set_tcp_nodelay(fd);
    return fd;
}

/* Egress UDP：创建 unconnected datagram socket。首目标仅用于校验可解析
 * 并决定 socket family（IPv4/IPv6），不 connect——后续 UDP_DATA 按帧自带
 * 地址用 sendto 发送，入站用 recvfrom 取真实来源。 */
static int udp_open_unbound(const char *host, uint16_t port) {
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    int fd;
    if (!bridge_resolve_endpoint_cached(host, port, &addr, &addr_len)) {
        return -1;
    }
    fd = (int) socket(addr.ss_family, SOCK_DGRAM, 0);
    if (fd == XGW_INVALID_SOCKET) {
        return -1;
    }
    if (!xgw_set_nonblocking(fd)) {
        xgw_close_socket(fd);
        return -1;
    }
#ifdef _WIN32
    /* 关闭 Windows 上 unconnected UDP 因前一次 sendto 触发 ICMP 不可达而在
     * 下一次 recvfrom 返回 WSAECONNRESET(10054) 的行为，避免误判为致命错误。 */
    {
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000CUL /* _WSAIOW(IOC_VENDOR, 12) */
#endif
        DWORD off = 0;
        DWORD bytes = 0;
        (void) WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &bytes, NULL, NULL);
    }
#endif
    return fd;
}

/* sockaddr -> "host:port" 文本。IPv6 用 "[host]:port" 形式，保证 Go 端
 * net.SplitHostPort / net.ResolveUDPAddr 解析无歧义。 */
static int bridge_sockaddr_to_text(const struct sockaddr_storage *addr,
                                   socklen_t addr_len,
                                   char *out, size_t out_cap) {
    char host[INET6_ADDRSTRLEN];
    (void) addr_len;
    if (addr == NULL || out == NULL || out_cap == 0U) {
        return 0;
    }
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *) addr;
        if (inet_ntop(AF_INET, (void *) &sin->sin_addr, host, sizeof(host)) == NULL) {
            return 0;
        }
        snprintf(out, out_cap, "%s:%u", host, (unsigned int) ntohs(sin->sin_port));
        return 1;
    }
#ifdef AF_INET6
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) addr;
        if (inet_ntop(AF_INET6, (void *) &sin6->sin6_addr, host, sizeof(host)) == NULL) {
            return 0;
        }
        snprintf(out, out_cap, "[%s]:%u", host, (unsigned int) ntohs(sin6->sin6_port));
        return 1;
    }
#endif
    return 0;
}

/* "host:port" 文本拆分。支持 "[v6]:port"、"v4:port"、"host:port"。
 * 取最后一个冒号作为端口分隔，剥除 IPv6 字面量的方括号。 */
static int bridge_split_hostport(const char *text,
                                 char *host_out, size_t host_cap,
                                 uint16_t *port_out) {
    const char *colon;
    const char *host_start = text;
    size_t host_len;
    unsigned long port_val;
    char *endp = NULL;
    if (text == NULL || host_out == NULL || host_cap == 0U || port_out == NULL) {
        return 0;
    }
    if (text[0] == '[') {
        const char *rb = strrchr(text, ']');
        if (rb == NULL || rb[1] != ':') {
            return 0;
        }
        host_start = text + 1;
        host_len = (size_t) (rb - host_start);
        colon = rb + 1;
    } else {
        colon = strrchr(text, ':');
        if (colon == NULL) {
            return 0;
        }
        host_len = (size_t) (colon - text);
    }
    if (host_len == 0U || host_len >= host_cap) {
        return 0;
    }
    memcpy(host_out, host_start, host_len);
    host_out[host_len] = '\0';
    port_val = strtoul(colon + 1, &endp, 10);
    if (endp == NULL || *endp != '\0' || port_val == 0UL || port_val > 65535UL) {
        return 0;
    }
    *port_out = (uint16_t) port_val;
    return 1;
}

static int write_status(int fd, uint8_t ok, const char *message) {
    uint16_t msg_len = (uint16_t) (message == NULL ? 0U : strlen(message));
    uint8_t head[3];
    head[0] = ok;
    head[1] = (uint8_t) (msg_len >> 8U);
    head[2] = (uint8_t) msg_len;
    if (!send_all(fd, head, sizeof(head))) {
        return 0;
    }
    if (msg_len > 0U) {
        if (!send_all(fd, (const uint8_t *) message, msg_len)) {
            return 0;
        }
    }
    return 1;
}

static uint16_t socket_local_port(int fd) {
    struct sockaddr_storage addr;
    socklen_arg_t len = (socklen_arg_t) sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (getsockname(fd, (struct sockaddr *) &addr, &len) != 0) {
        return 0U;
    }
    if (addr.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in *) &addr)->sin_port);
    }
#ifdef AF_INET6
    if (addr.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6 *) &addr)->sin6_port);
    }
#endif
    return 0U;
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24U) |
           ((uint32_t) p[1] << 16U) |
           ((uint32_t) p[2] << 8U) |
           (uint32_t) p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t) (((uint16_t) p[0] << 8U) | (uint16_t) p[1]);
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24U);
    p[1] = (uint8_t) (v >> 16U);
    p[2] = (uint8_t) (v >> 8U);
    p[3] = (uint8_t) v;
}

static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8U);
    p[1] = (uint8_t) v;
}

static void decode_bridge_meta_compat(const uint8_t *src, size_t src_len, xgw_bridge_flow_meta_t *meta) {
    if (meta == NULL || src == NULL || src_len < XGW_BRIDGE_META_LEN_V1) {
        return;
    }
    meta->flow_class = src[0];
    meta->priority = src[1];
    meta->budget_flags = src[2];
    meta->preferred_copies = src[3] == 0U ? 1U : src[3];
    if (src_len >= XGW_BRIDGE_META_LEN) {
        meta->read_timeout_ms = read_be32(src + 4U);
        meta->idle_after_first_byte_ms = read_be32(src + 8U);
    } else {
        meta->read_timeout_ms = read_be16(src + 4U);
        meta->idle_after_first_byte_ms = read_be16(src + 6U);
    }
}

static void decode_bridge_session_meta_compat(const uint8_t *src, size_t src_len, xgw_bridge_session_meta_t *meta) {
    if (meta == NULL) {
        return;
    }
    bridge_default_session_meta(meta);
    if (src == NULL || src_len == 0U) {
        return;
    }
    if (src_len >= 40U) {
        memcpy(meta->session_id, src, 39U);
        meta->session_id[39] = '\0';
    }
    if (src_len >= 56U) {
        memcpy(meta->frontend, src + 40U, 15U);
        meta->frontend[15] = '\0';
    }
    if (src_len >= 80U) {
        memcpy(meta->route_name, src + 56U, 23U);
        meta->route_name[23] = '\0';
    }
    if (src_len >= 96U) {
        memcpy(meta->line_id, src + 80U, 15U);
        meta->line_id[15] = '\0';
    }
}

static uint32_t read_le32(const uint8_t *p) {
    return ((uint32_t) p[0]) |
           ((uint32_t) p[1] << 8U) |
           ((uint32_t) p[2] << 16U) |
           ((uint32_t) p[3] << 24U);
}

static uint64_t read_le64(const uint8_t *p) {
    uint64_t lo = read_le32(p);
    uint64_t hi = read_le32(p + 4U);
    return lo | (hi << 32U);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8U);
    p[2] = (uint8_t) (v >> 16U);
    p[3] = (uint8_t) (v >> 24U);
}

static void write_le64(uint8_t *p, uint64_t v) {
    write_le32(p, (uint32_t) v);
    write_le32(p + 4U, (uint32_t) (v >> 32U));
}

static void ring_bump_notify(xgw_bridge_ring_file_t *ring) {
    uint32_t next;
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE) {
        return;
    }
    next = read_le32(ring->map + XGW_BRIDGE_RING_NOTIFY_OFFSET) + 1U;
    write_le32(ring->map + XGW_BRIDGE_RING_NOTIFY_OFFSET, next);
    ring_notify_wake((uint32_t *) (void *) (ring->map + XGW_BRIDGE_RING_NOTIFY_OFFSET));
}

/* Snapshot volatile header fields from the live mmap. Pure read: it does NOT
 * touch attached_generation; attach/reattach own epoch transitions. */
static void ring_read_header_fields(xgw_bridge_ring_file_t *ring) {
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE) {
        return;
    }
    ring->flags = read_le32(ring->map + XGW_BRIDGE_RING_FLAGS_OFFSET);
    ring->generation = read_le64(ring->map + XGW_BRIDGE_RING_GENERATION_OFFSET);
    ring->writer_role = read_le32(ring->map + XGW_BRIDGE_RING_WRITER_ROLE_OFFSET);
    ring->reader_role = read_le32(ring->map + XGW_BRIDGE_RING_READER_ROLE_OFFSET);
}

/* attach: join the live epoch — adopt the header generation as our baseline and
 * publish readiness + liveness. Idempotent within an epoch; the mmap is never
 * touched here (no reopen). This is the C twin of bridgeRingFile.attach(). */
static void ring_attach(xgw_bridge_ring_file_t *ring) {
    uint32_t flags;
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE) {
        return;
    }
    ring_read_header_fields(ring);
    ring->attached_generation = ring->generation;
    flags = read_le32(ring->map + XGW_BRIDGE_RING_FLAGS_OFFSET);
    flags |= XGW_BRIDGE_RING_FLAG_OWNER_READY | ring->local_ready_flag;
    write_le32(ring->map + XGW_BRIDGE_RING_FLAGS_OFFSET, flags);
    write_le32(ring->map + ring->local_attach_offset, 1U);
}

static int ring_generation_changed(const xgw_bridge_ring_file_t *ring) {
    uint64_t current;
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE ||
        ring->attached_generation == 0U) {
        return 0;
    }
    current = read_le64(ring->map + XGW_BRIDGE_RING_GENERATION_OFFSET);
    return current != 0U && current != ring->attached_generation;
}

/* ensure_attached: steady-state membership keep-alive. Reattach in place if the
 * peer bumped the generation; otherwise only re-assert the attach bit if it was
 * cleared — no unconditional per-frame header writes. Returns 1 if an in-place
 * reattach happened. Never reopens; the mmap is preserved across peer restarts. */
static int ring_ensure_attached(xgw_bridge_ring_file_t *ring) {
    uint64_t current;
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE) {
        return 0;
    }
    current = read_le64(ring->map + XGW_BRIDGE_RING_GENERATION_OFFSET);
    if (ring->attached_generation == 0U || (current != 0U && current != ring->attached_generation)) {
        ring_attach(ring);
        return ring->attached_generation != 0U;
    }
    if (read_le32(ring->map + ring->local_attach_offset) == 0U) {
        write_le32(ring->map + ring->local_attach_offset, 1U);
    }
    return 0;
}

static void ring_mark_detached(xgw_bridge_ring_file_t *ring) {
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE) {
        return;
    }
    write_le32(ring->map + ring->local_attach_offset, 0U);
}

static int ring_peer_ready(const xgw_bridge_ring_file_t *ring) {
    uint32_t flags;
    uint32_t attached;
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE) {
        return 0;
    }
    flags = read_le32(ring->map + XGW_BRIDGE_RING_FLAGS_OFFSET);
    attached = read_le32(ring->map + ring->peer_attach_offset);
    return (flags & ring->peer_ready_flag) != 0U && attached != 0U;
}

static long ring_slot_offset(uint64_t seq, uint32_t capacity, uint32_t slot_size) {
    uint64_t idx = seq % (uint64_t) capacity;
    return (long) XGW_BRIDGE_RING_HEADER_SIZE + (long) idx * (long) slot_size;
}

static int ring_map_file(xgw_bridge_ring_file_t *ring, size_t total_size) {
    if (ring == NULL || ring->fp == NULL || total_size == 0U) {
        return 0;
    }
#ifdef _WIN32
    ring->map_handle = CreateFileMappingA((HANDLE) _get_osfhandle(_fileno(ring->fp)),
                                          NULL,
                                          PAGE_READWRITE,
                                          (DWORD) (((uint64_t) total_size) >> 32U),
                                          (DWORD) total_size,
                                          NULL);
    if (ring->map_handle == NULL) {
        return 0;
    }
    ring->map = (uint8_t *) MapViewOfFile(ring->map_handle,
                                          FILE_MAP_READ | FILE_MAP_WRITE,
                                          0,
                                          0,
                                          total_size);
    if (ring->map == NULL) {
        CloseHandle(ring->map_handle);
        ring->map_handle = NULL;
        return 0;
    }
#else
    ring->map = (uint8_t *) mmap(NULL,
                                 total_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED,
                                 fileno(ring->fp),
                                 0);
    if (ring->map == MAP_FAILED) {
        ring->map = NULL;
        return 0;
    }
#endif
    ring->map_len = total_size;
    return 1;
}

static void ring_unmap_file(xgw_bridge_ring_file_t *ring) {
    if (ring == NULL || ring->map == NULL) {
        return;
    }
#ifdef _WIN32
    UnmapViewOfFile(ring->map);
    if (ring->map_handle != NULL) {
        CloseHandle(ring->map_handle);
        ring->map_handle = NULL;
    }
#else
    munmap(ring->map, ring->map_len);
#endif
    ring->map = NULL;
    ring->map_len = 0U;
}

static int ring_read_header(xgw_bridge_ring_file_t *ring,
                            uint8_t *header,
                            uint64_t *write_seq,
                            uint64_t *read_seq) {
    if (ring == NULL || ring->map == NULL || ring->map_len < XGW_BRIDGE_RING_HEADER_SIZE) {
        return 0;
    }
    memcpy(header, ring->map, XGW_BRIDGE_RING_HEADER_SIZE);
    if (read_le32(header) != XGW_BRIDGE_RING_MAGIC) {
        return 0;
    }
    if (read_le32(header + 4U) != XGW_BRIDGE_RING_VERSION) {
        return 0;
    }
    ring->capacity = read_le32(header + 8U);
    ring->slot_size = read_le32(header + 12U);
    if (ring->capacity == 0U || ring->slot_size < XGW_BRIDGE_RING_SLOT_HEAD) {
        return 0;
    }
    ring->flags = read_le32(header + XGW_BRIDGE_RING_FLAGS_OFFSET);
    ring->generation = read_le64(header + XGW_BRIDGE_RING_GENERATION_OFFSET);
    ring->writer_role = read_le32(header + XGW_BRIDGE_RING_WRITER_ROLE_OFFSET);
    ring->reader_role = read_le32(header + XGW_BRIDGE_RING_READER_ROLE_OFFSET);
    if (write_seq != NULL) {
        *write_seq = read_le64(header + XGW_BRIDGE_RING_WRITE_SEQ_OFFSET);
    }
    if (read_seq != NULL) {
        *read_seq = read_le64(header + XGW_BRIDGE_RING_READ_SEQ_OFFSET);
    }
    return 1;
}

/* 按文件名（.go2c / 其它）确定本端在 ring 上的读写角色与就绪标志位。 */
static void ring_setup_roles(xgw_bridge_ring_file_t *ring, const char *path) {
    if (strstr(path, ".go2c") != NULL) {
        ring->writer_role = XGW_BRIDGE_RING_ROLE_FRONT;
        ring->reader_role = XGW_BRIDGE_RING_ROLE_INGRESS;
        ring->local_ready_flag = XGW_BRIDGE_RING_FLAG_READER_READY;
        ring->peer_ready_flag = XGW_BRIDGE_RING_FLAG_WRITER_READY;
        ring->local_attach_offset = XGW_BRIDGE_RING_READER_ATTACH_OFFSET;
        ring->peer_attach_offset = XGW_BRIDGE_RING_WRITER_ATTACH_OFFSET;
    } else {
        ring->writer_role = XGW_BRIDGE_RING_ROLE_INGRESS;
        ring->reader_role = XGW_BRIDGE_RING_ROLE_FRONT;
        ring->local_ready_flag = XGW_BRIDGE_RING_FLAG_WRITER_READY;
        ring->peer_ready_flag = XGW_BRIDGE_RING_FLAG_READER_READY;
        ring->local_attach_offset = XGW_BRIDGE_RING_WRITER_ATTACH_OFFSET;
        ring->peer_attach_offset = XGW_BRIDGE_RING_READER_ATTACH_OFFSET;
    }
}

/* 判断已打开文件是否是一个布局合法、大小匹配的既有 ring。 */
static int ring_file_is_valid_existing(xgw_bridge_ring_file_t *ring,
                                       const uint8_t *header,
                                       long current_size,
                                       long total_size) {
    (void) ring;
    if (current_size < (long) XGW_BRIDGE_RING_HEADER_SIZE || current_size != total_size) {
        return 0;
    }
    return read_le32(header) == XGW_BRIDGE_RING_MAGIC &&
           read_le32(header + 4U) == XGW_BRIDGE_RING_VERSION &&
           read_le32(header + 8U) == XGW_BRIDGE_RING_CAPACITY &&
           read_le32(header + 12U) == XGW_BRIDGE_RING_SLOT_SIZE;
}

/* attach 既有 ring：仅 map 并加入对端已发布的 epoch，不改写头部/generation。 */
static int ring_attach_existing_file(xgw_bridge_ring_file_t *ring, long total_size) {
    if (!ring_map_file(ring, (size_t) total_size)) {
        return 0;
    }
    ring_attach(ring);
    return 1;
}

/* 初始化新 ring：撑大文件、写入全新头部与 generation（开启新 epoch），再 map + attach。 */
static int ring_create_new_file(xgw_bridge_ring_file_t *ring, long total_size) {
    uint8_t header[XGW_BRIDGE_RING_HEADER_SIZE];
    if (fseek(ring->fp, total_size - 1L, SEEK_SET) != 0 || fputc(0, ring->fp) == EOF) {
        return 0;
    }
    memset(header, 0, sizeof(header));
    write_le32(header, XGW_BRIDGE_RING_MAGIC);
    write_le32(header + 4U, XGW_BRIDGE_RING_VERSION);
    write_le32(header + 8U, XGW_BRIDGE_RING_CAPACITY);
    write_le32(header + 12U, XGW_BRIDGE_RING_SLOT_SIZE);
    write_le32(header + XGW_BRIDGE_RING_FLAGS_OFFSET, XGW_BRIDGE_RING_FLAG_OWNER_READY);
    write_le64(header + XGW_BRIDGE_RING_GENERATION_OFFSET, xgw_bridge_now_us());
    write_le32(header + XGW_BRIDGE_RING_WRITER_ROLE_OFFSET, ring->writer_role);
    write_le32(header + XGW_BRIDGE_RING_READER_ROLE_OFFSET, ring->reader_role);
    if (fseek(ring->fp, 0L, SEEK_SET) != 0 ||
        fwrite(header, 1U, sizeof(header), ring->fp) != sizeof(header)) {
        return 0;
    }
    if (!ring_map_file(ring, (size_t) total_size)) {
        return 0;
    }
    ring_attach(ring);
    return 1;
}

/* 打开 ring 文件并分派：合法既有 ring 走 attach 路径（保留对端 epoch），
 * 否则走新建路径（写新头部 + 新 generation）。两条路径语义分离、互不混淆。 */
static int ring_init_file(xgw_bridge_ring_file_t *ring, const char *path) {
    uint8_t header[XGW_BRIDGE_RING_HEADER_SIZE];
    long total_size;
    long current_size = 0L;
    int is_existing = 0;
    if (ring == NULL || path == NULL || path[0] == '\0') {
        return 0;
    }
    memset(ring, 0, sizeof(*ring));
    snprintf(ring->path, sizeof(ring->path), "%s", path);
    ring_setup_roles(ring, path);
    ensure_parent_dir(path);
    ring->fp = fopen(path, "r+b");
    if (ring->fp == NULL) {
        ring->fp = fopen(path, "w+b");
    }
    if (ring->fp == NULL) {
        return 0;
    }
    (void) setvbuf(ring->fp, NULL, _IONBF, 0);
    ring->capacity = XGW_BRIDGE_RING_CAPACITY;
    ring->slot_size = XGW_BRIDGE_RING_SLOT_SIZE;
    total_size = (long) XGW_BRIDGE_RING_HEADER_SIZE +
                 (long) XGW_BRIDGE_RING_CAPACITY * (long) XGW_BRIDGE_RING_SLOT_SIZE;
    if (fseek(ring->fp, 0L, SEEK_END) != 0) {
        fclose(ring->fp);
        ring->fp = NULL;
        return 0;
    }
    current_size = ftell(ring->fp);
    if (current_size < 0L) {
        fclose(ring->fp);
        ring->fp = NULL;
        return 0;
    }
    /* 探测：读出头部并判断是否为合法既有 ring。 */
    if (current_size >= (long) sizeof(header)) {
        if (fseek(ring->fp, 0L, SEEK_SET) != 0 ||
            fread(header, 1U, sizeof(header), ring->fp) != sizeof(header)) {
            fclose(ring->fp);
            ring->fp = NULL;
            return 0;
        }
        is_existing = ring_file_is_valid_existing(ring, header, current_size, total_size);
    }
    if (is_existing) {
        if (!ring_attach_existing_file(ring, total_size)) {
            fclose(ring->fp);
            ring->fp = NULL;
            return 0;
        }
        return 1;
    }
    if (!ring_create_new_file(ring, total_size)) {
        fclose(ring->fp);
        ring->fp = NULL;
        return 0;
    }
    return 1;
}

static void ring_close_file(xgw_bridge_ring_file_t *ring) {
    ring_mark_detached(ring);
    ring_unmap_file(ring);
    if (ring != NULL && ring->fp != NULL) {
        fclose(ring->fp);
        ring->fp = NULL;
    }
}

static int ring_write_slot(xgw_bridge_ring_file_t *ring,
                           uint64_t write_seq,
                           uint8_t kind,
                           uint32_t client_id,
                           const uint8_t *body,
                           size_t body_len) {
    uint8_t *slot;
    uint8_t *frame;
    size_t frame_len = 5U + body_len;
    long offset;
    if (ring == NULL || frame_len > XGW_BRIDGE_RING_FRAME_MAX) {
        return 0;
    }
    offset = ring_slot_offset(write_seq, ring->capacity, ring->slot_size);
    if (offset < 0 || (size_t) offset + XGW_BRIDGE_RING_SLOT_HEAD + frame_len > ring->map_len) {
        return 0;
    }
    slot = ring->map + offset;
    memset(slot, 0, XGW_BRIDGE_RING_SLOT_HEAD);
    write_le64(slot, write_seq + 1U);
    write_le32(slot + 8U, (uint32_t) frame_len);
    frame = slot + XGW_BRIDGE_RING_SLOT_HEAD;
    frame[0] = kind;
    write_be32(frame + 1U, client_id);
    if (body_len > 0U && body != NULL) {
        memcpy(frame + 5U, body, body_len);
    }
    return 1;
}

static int ring_push_frame(xgw_bridge_ring_file_t *ring,
                           uint8_t kind,
                           uint32_t client_id,
                           const uint8_t *body,
                           size_t body_len) {
    uint8_t header[XGW_BRIDGE_RING_HEADER_SIZE];
    uint64_t write_seq = 0U;
    uint64_t read_seq = 0U;
    if (5U + body_len > XGW_BRIDGE_RING_FRAME_MAX ||
        !ring_read_header(ring, header, &write_seq, &read_seq)) {
        return -1;
    }
    /* Generation bump = peer republished a new epoch. Reattach in place (mmap
     * preserved); do NOT reopen. Reopen is reserved for structural corruption,
     * which ring_read_header already rejects above. */
    if (ring_generation_changed(ring)) {
        ring_attach(ring);
    } else {
        (void) ring_ensure_attached(ring);
    }
    if (!ring_peer_ready(ring)) {
        printf("bridge.ring.push.peer_not_ready path=%s flags=%u generation=%llu\n",
               ring->path,
               (unsigned int) ring->flags,
               (unsigned long long) ring->generation);
        fflush(stdout);
        return 0;
    }
    if (write_seq - read_seq >= (uint64_t) ring->capacity) {
        return 0;
    }
    if (!ring_write_slot(ring, write_seq, kind, client_id, body, body_len)) {
        return -1;
    }
    write_le64(ring->map + XGW_BRIDGE_RING_WRITE_SEQ_OFFSET, write_seq + 1U);
    ring_bump_notify(ring);
    return 1;
}

static size_t encode_open_payload(const xgw_bridge_client_t *client, uint8_t *buf, size_t cap) {
    size_t host_len = strlen(client->target_host);
    if (cap < 13U + host_len + XGW_BRIDGE_META_LEN + XGW_BRIDGE_SESSION_META_LEN) {
        return 0U;
    }
    write_be32(buf, XGW_BRIDGE_MAGIC);
    write_be32(buf + 4U, client->stream_id);
    buf[8] = client->proto == XGW_BRIDGE_PROTO_UDP ? XGW_BRIDGE_KIND_UDP_OPEN : XGW_BRIDGE_KIND_TCP_OPEN;
    write_be16(buf + 9U, (uint16_t) host_len);
    memcpy(buf + 11U, client->target_host, host_len);
    write_be16(buf + 11U + host_len, client->target_port);
    buf[13U + host_len + 0U] = client->meta.flow_class;
    buf[13U + host_len + 1U] = client->meta.priority;
    buf[13U + host_len + 2U] = client->meta.budget_flags;
    buf[13U + host_len + 3U] = client->meta.preferred_copies == 0U ? 1U : client->meta.preferred_copies;
    write_be32(buf + 13U + host_len + 4U, client->meta.read_timeout_ms);
    write_be32(buf + 13U + host_len + 8U, client->meta.idle_after_first_byte_ms);
    memset(buf + 13U + host_len + XGW_BRIDGE_META_LEN, 0, XGW_BRIDGE_SESSION_META_LEN);
    snprintf((char *) (buf + 13U + host_len + XGW_BRIDGE_META_LEN + 0U), 40U, "%s", client->session_meta.session_id);
    snprintf((char *) (buf + 13U + host_len + XGW_BRIDGE_META_LEN + 40U), 16U, "%s", client->session_meta.frontend);
    snprintf((char *) (buf + 13U + host_len + XGW_BRIDGE_META_LEN + 56U), 24U, "%s", client->session_meta.route_name);
    snprintf((char *) (buf + 13U + host_len + XGW_BRIDGE_META_LEN + 80U), 16U, "%s", client->session_meta.line_id);
    return 13U + host_len + XGW_BRIDGE_META_LEN + XGW_BRIDGE_SESSION_META_LEN;
}

static size_t encode_data_payload(uint32_t stream_id, const uint8_t *data, size_t data_len, uint8_t *buf, size_t cap) {
    if (cap < 11U + data_len) {
        return 0U;
    }
    write_be32(buf, XGW_BRIDGE_MAGIC);
    write_be32(buf + 4U, stream_id);
    buf[8] = XGW_BRIDGE_KIND_TCP_DATA;
    write_be16(buf + 9U, (uint16_t) data_len);
    memcpy(buf + 11U, data, data_len);
    return 11U + data_len;
}

static size_t encode_udp_data_payload(uint32_t stream_id,
                                      const char *addr,
                                      const uint8_t *data,
                                      size_t data_len,
                                      uint8_t *buf,
                                      size_t cap) {
    size_t addr_len = addr == NULL ? 0U : strlen(addr);
    if (addr_len == 0U || addr_len > 65535U || data_len > 65535U || cap < 13U + addr_len + data_len) {
        return 0U;
    }
    write_be32(buf, XGW_BRIDGE_MAGIC);
    write_be32(buf + 4U, stream_id);
    buf[8] = XGW_BRIDGE_KIND_UDP_DATA;
    write_be16(buf + 9U, (uint16_t) addr_len);
    memcpy(buf + 11U, addr, addr_len);
    write_be16(buf + 11U + addr_len, (uint16_t) data_len);
    memcpy(buf + 13U + addr_len, data, data_len);
    return 13U + addr_len + data_len;
}

static int decode_udp_front_frame(xgw_bridge_client_t *client,
                                  uint8_t *buf,
                                  size_t buf_cap,
                                  size_t *out_len) {
    uint16_t addr_len;
    uint16_t data_len;
    size_t total_len;
    char addr[256];
    if (client == NULL || out_len == NULL) {
        return -1;
    }
    *out_len = 0U;
    if (client->udp_rx_len < 4U) {
        return 0;
    }
    addr_len = read_be16(client->udp_rx_buf);
    if (addr_len == 0U || addr_len >= sizeof(addr)) {
        return -1;
    }
    if (client->udp_rx_len < 4U + addr_len) {
        return 0;
    }
    data_len = read_be16(client->udp_rx_buf + 2U + addr_len);
    total_len = 4U + addr_len + data_len;
    if (client->udp_rx_len < total_len) {
        return 0;
    }
    memcpy(addr, client->udp_rx_buf + 2U, addr_len);
    addr[addr_len] = '\0';
    *out_len = encode_udp_data_payload(client->stream_id,
                                       addr,
                                       client->udp_rx_buf + 4U + addr_len,
                                       data_len,
                                       buf,
                                       buf_cap);
    if (*out_len == 0U) {
        return -1;
    }
    memmove(client->udp_rx_buf, client->udp_rx_buf + total_len, client->udp_rx_len - total_len);
    client->udp_rx_len -= total_len;
    return 1;
}

static size_t encode_close_payload(uint32_t stream_id, uint8_t *buf, size_t cap) {
    if (cap < 9U) {
        return 0U;
    }
    write_be32(buf, XGW_BRIDGE_MAGIC);
    write_be32(buf + 4U, stream_id);
    buf[8] = XGW_BRIDGE_KIND_TCP_CLOSE;
    return 9U;
}

static size_t encode_half_close_payload(uint32_t stream_id, uint8_t *buf, size_t cap) {
    if (cap < 9U) {
        return 0U;
    }
    write_be32(buf, XGW_BRIDGE_MAGIC);
    write_be32(buf + 4U, stream_id);
    buf[8] = XGW_BRIDGE_KIND_TCP_HALF_CLOSE;
    return 9U;
}

static xgw_bridge_client_t *find_client_by_ring_id(xgw_bridge_server_t *server, uint32_t ring_client_id) {
    size_t i;
    for (i = 0; i < XGW_BRIDGE_MAX_CLIENTS; ++i) {
        if (server->clients[i].active &&
            server->clients[i].is_ring &&
            server->clients[i].ring_client_id == ring_client_id) {
            return &server->clients[i];
        }
    }
    return NULL;
}

static xgw_bridge_client_t *find_client_by_stream(xgw_bridge_server_t *server, uint32_t stream_id) {
    size_t i;
    for (i = 0; i < XGW_BRIDGE_MAX_CLIENTS; ++i) {
        if (server->clients[i].active && server->clients[i].stream_id == stream_id) {
            return &server->clients[i];
        }
    }
    return NULL;
}

static void close_client_with_reason(xgw_bridge_client_t *client, const char *reason) {
    if (client == NULL || !client->active) {
        return;
    }
    bridge_client_set_close_reason(client, reason);
    bridge_client_log_close(client);
    if (client->fd >= 0) {
        xgw_close_socket(client->fd);
    }
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static int mark_client_front_eof(xgw_bridge_client_t *client) {
    if (client == NULL || !client->active) {
        return 0;
    }
    client->front_eof = 1;
    return 0;
}

static void close_client(xgw_bridge_client_t *client) {
    close_client_with_reason(client, NULL);
}

static xgw_bridge_client_t *allocate_client(xgw_bridge_server_t *server) {
    size_t i;
    for (i = 0; i < XGW_BRIDGE_MAX_CLIENTS; ++i) {
        if (!server->clients[i].active) {
            memset(&server->clients[i], 0, sizeof(server->clients[i]));
            server->clients[i].active = 1;
            server->clients[i].fd = -1;
            server->clients[i].stream_id = ++server->next_stream_id;
            server->clients[i].accepted_us = xgw_bridge_now_us();
            server->clients[i].priority = XGW_BRIDGE_PRIORITY_NORMAL;
            return &server->clients[i];
        }
    }
    return NULL;
}

static int bridge_pending_push_ex(xgw_bridge_server_t *server, const uint8_t *payload, size_t payload_len, uint32_t priority) {
    size_t idx;
    if (server == NULL || payload == NULL || payload_len == 0U ||
        payload_len > XGW_BRIDGE_WIRE_PAYLOAD_MAX ||
        server->pending_count >= XGW_BRIDGE_PENDING_PAYLOADS) {
        return 0;
    }
    idx = (server->pending_head + server->pending_count) % XGW_BRIDGE_PENDING_PAYLOADS;
    memcpy(server->pending[idx].data, payload, payload_len);
    server->pending[idx].len = payload_len;
    server->pending[idx].priority = priority;
    server->pending_count++;
    return 1;
}

static int bridge_pending_pop(xgw_bridge_server_t *server,
                              uint8_t *buf,
                              size_t buf_cap,
                              size_t *out_len) {
    size_t len;
    size_t i;
    size_t best = 0U;
    uint32_t best_priority = 0U;
    if (server == NULL || buf == NULL || out_len == NULL || server->pending_count == 0U) {
        return 0;
    }
    best = server->pending_head;
    best_priority = server->pending[best].priority;
    for (i = 1U; i < server->pending_count; ++i) {
        size_t idx = (server->pending_head + i) % XGW_BRIDGE_PENDING_PAYLOADS;
        if (server->pending[idx].priority > best_priority) {
            best = idx;
            best_priority = server->pending[idx].priority;
        }
    }
    len = server->pending[best].len;
    if (len > buf_cap) {
        return -1;
    }
    memcpy(buf, server->pending[best].data, len);
    *out_len = len;
    while (best != server->pending_head) {
        size_t prev = (best + XGW_BRIDGE_PENDING_PAYLOADS - 1U) % XGW_BRIDGE_PENDING_PAYLOADS;
        server->pending[best] = server->pending[prev];
        best = prev;
    }
    server->pending[server->pending_head].len = 0U;
    server->pending[server->pending_head].priority = 0U;
    server->pending_head = (server->pending_head + 1U) % XGW_BRIDGE_PENDING_PAYLOADS;
    server->pending_count--;
    return 1;
}

static int ring_write_frame(xgw_bridge_server_t *server, uint8_t kind, uint32_t client_id, const uint8_t *body, size_t body_len) {
    size_t frame_len = 5U + body_len;
    int rc;
    if (server == NULL || frame_len > XGW_BRIDGE_RING_FRAME_MAX) {
        return 0;
    }
    rc = ring_push_frame(&server->ring_tx, kind, client_id, body, body_len);
    return rc > 0;
}

static int ring_write_frame_retry(xgw_bridge_server_t *server,
                                  uint8_t kind,
                                  uint32_t client_id,
                                  const uint8_t *body,
                                  size_t body_len,
                                  uint64_t timeout_us) {
    uint64_t deadline_us;
    if (server == NULL) {
        return 0;
    }
    deadline_us = xgw_bridge_now_us() + timeout_us;
    do {
        if (ring_write_frame(server, kind, client_id, body, body_len)) {
            return 1;
        }
        xgw_bridge_sleep_ms(1U);
    } while (xgw_bridge_now_us() < deadline_us);
    printf("bridge.ring.status.retry_fail client=%u kind=%u timeout_us=%llu\n",
           client_id,
           (unsigned int) kind,
           (unsigned long long) timeout_us);
    fflush(stdout);
    return 0;
}

static int ring_write_status(xgw_bridge_server_t *server, uint32_t client_id, uint8_t ok, const char *message) {
    uint8_t body[512];
    size_t msg_len = message == NULL ? 0U : strlen(message);
    if (msg_len > 509U) {
        msg_len = 509U;
    }
    body[0] = ok;
    write_be16(body + 1U, (uint16_t) msg_len);
    if (msg_len > 0U) {
        memcpy(body + 3U, message, msg_len);
    }
    printf("bridge.ring.status client=%u ok=%u msg=%.*s\n",
           client_id,
           (unsigned int) ok,
           (int) msg_len,
           message == NULL ? "" : message);
    fflush(stdout);
    return ring_write_frame_retry(server, XGW_BRIDGE_RING_KIND_STATUS, client_id, body, 3U + msg_len, 2000000ULL);
}

static int parse_ring_open_body(const uint8_t *body,
                                size_t body_len,
                                xgw_bridge_proto_t *proto,
                                char *host,
                                size_t host_cap,
                                uint16_t *port,
                                xgw_bridge_flow_meta_t *meta,
                                xgw_bridge_session_meta_t *session_meta) {
    uint16_t host_len;
    if (body == NULL || proto == NULL || host == NULL || port == NULL || body_len < 5U) {
        return 0;
    }
    if (body[0] == XGW_BRIDGE_PROTO_UDP) {
        *proto = XGW_BRIDGE_PROTO_UDP;
    } else if (body[0] == XGW_BRIDGE_PROTO_TCP) {
        *proto = XGW_BRIDGE_PROTO_TCP;
    } else {
        return 0;
    }
    host_len = read_be16(body + 1U);
    if (host_len == 0U || host_len >= host_cap || body_len < 5U + host_len) {
        return 0;
    }
    memcpy(host, body + 3U, host_len);
    host[host_len] = '\0';
    *port = read_be16(body + 3U + host_len);
    bridge_default_meta(meta, XGW_BRIDGE_PRIORITY_NORMAL);
    bridge_default_session_meta(session_meta);
    if (meta != NULL && body_len >= 5U + host_len + XGW_BRIDGE_META_LEN_V1) {
        const uint8_t *mb = body + 5U + host_len;
        size_t meta_len = body_len - (5U + host_len);
        if (meta_len > XGW_BRIDGE_META_LEN + XGW_BRIDGE_SESSION_META_LEN) {
            meta_len = XGW_BRIDGE_META_LEN + XGW_BRIDGE_SESSION_META_LEN;
        }
        decode_bridge_meta_compat(mb, meta_len >= XGW_BRIDGE_META_LEN ? XGW_BRIDGE_META_LEN : XGW_BRIDGE_META_LEN_V1, meta);
        if (session_meta != NULL && meta_len > XGW_BRIDGE_META_LEN) {
            decode_bridge_session_meta_compat(mb + XGW_BRIDGE_META_LEN, meta_len - XGW_BRIDGE_META_LEN, session_meta);
        }
    }
    return 1;
}

static int handle_ring_frame(xgw_bridge_server_t *server, const uint8_t *frame, size_t frame_len) {
    uint8_t kind;
    uint32_t client_id;
    const uint8_t *body;
    size_t body_len;
    xgw_bridge_client_t *client;
    if (server == NULL || frame == NULL || frame_len < 5U) {
        return 0;
    }
    kind = frame[0];
    client_id = read_be32(frame + 1U);
    body = frame + 5U;
    body_len = frame_len - 5U;
    if (kind == XGW_BRIDGE_RING_KIND_TCP_OPEN || kind == XGW_BRIDGE_RING_KIND_UDP_OPEN) {
        xgw_bridge_proto_t proto = XGW_BRIDGE_PROTO_TCP;
        char host[256];
        uint16_t port = 0U;
        xgw_bridge_flow_meta_t meta;
        xgw_bridge_session_meta_t session_meta;
        uint8_t payload[XGW_BRIDGE_WIRE_PAYLOAD_MAX];
        size_t payload_len;
        if (!parse_ring_open_body(body, body_len, &proto, host, sizeof(host), &port, &meta, &session_meta)) {
            printf("bridge.ring.open.invalid client=%u kind=%u body_len=%zu\n",
                   client_id,
                   (unsigned int) kind,
                   body_len);
            fflush(stdout);
            (void) ring_write_status(server, client_id, 0U, "invalid shared-ring open");
            return 1;
        }
        printf("bridge.ring.open client=%u kind=%u proto=%s target=%s:%u body_len=%zu\n",
               client_id,
               (unsigned int) kind,
               proto == XGW_BRIDGE_PROTO_UDP ? "udp" : "tcp",
               host,
               port,
               body_len);
        fflush(stdout);
        client = allocate_client(server);
        if (client == NULL) {
            (void) ring_write_status(server, client_id, 0U, "bridge client table full");
            return 1;
        }
        client->is_ring = 1;
        client->ring_client_id = client_id;
        client->proto = proto;
        snprintf(client->target_host, sizeof(client->target_host), "%s", host);
        client->target_port = port;
        client->meta = meta;
        client->session_meta = session_meta;
        client->priority = client->meta.priority == 0U
                           ? bridge_classify_priority(client->target_host, client->target_port)
                           : (uint32_t) client->meta.priority;
        if (client->meta.read_timeout_ms == 0U || client->meta.idle_after_first_byte_ms == 0U) {
            bridge_apply_meta_defaults(client);
        }
        bridge_client_log_accept(client);
        payload_len = encode_open_payload(client, payload, sizeof(payload));
        if (payload_len == 0U || !bridge_pending_push_ex(server, payload, payload_len, client->priority)) {
            close_client_with_reason(client, "bridge_pending_queue_full");
            (void) ring_write_status(server, client_id, 0U, "bridge pending queue full");
            return 1;
        }
        client->open_sent = 1;
        (void) ring_write_status(server, client_id, 1U, "queued");
        printf("bridge.accept stream=%u proto=%s target=%s:%u transport=shared-ring client=%u\n",
               client->stream_id,
               client->proto == XGW_BRIDGE_PROTO_UDP ? "udp" : "tcp",
               client->target_host,
               client->target_port,
               client_id);
        fflush(stdout);
        return 1;
    }
    client = find_client_by_ring_id(server, client_id);
    if (client == NULL) {
        return 1;
    }
    if (kind == XGW_BRIDGE_RING_KIND_TCP_DATA) {
        uint8_t payload[XGW_BRIDGE_WIRE_PAYLOAD_MAX];
        size_t payload_len = encode_data_payload(client->stream_id, body, body_len, payload, sizeof(payload));
        if (payload_len == 0U) {
            return XGW_RING_FRAME_ERR; /* 编码失败 = 结构错误，致命 */
        }
        if (!bridge_pending_push_ex(server, payload, payload_len, client->priority)) {
            return XGW_RING_FRAME_BACKPRESSURE; /* 发送队列暂满，本轮停止、下轮重读 */
        }
        bridge_client_note_first_packet(client, body_len, "shared_ring");
        return XGW_RING_FRAME_OK;
    }
    if (kind == XGW_BRIDGE_RING_KIND_UDP_DATA) {
        uint16_t addr_len;
        uint16_t data_len;
        char addr[256];
        uint8_t payload[XGW_BRIDGE_WIRE_PAYLOAD_MAX];
        size_t payload_len;
        if (body_len < 4U) {
            return 0;
        }
        addr_len = read_be16(body);
        if (addr_len == 0U || addr_len >= sizeof(addr) || body_len < 4U + addr_len) {
            return 0;
        }
        data_len = read_be16(body + 2U + addr_len);
        if (body_len < 4U + addr_len + data_len) {
            return 0;
        }
        memcpy(addr, body + 2U, addr_len);
        addr[addr_len] = '\0';
        payload_len = encode_udp_data_payload(client->stream_id,
                                              addr,
                                              body + 4U + addr_len,
                                              data_len,
                                              payload,
                                              sizeof(payload));
        if (payload_len == 0U) {
            return XGW_RING_FRAME_ERR; /* 编码失败 = 结构错误，致命 */
        }
        if (!bridge_pending_push_ex(server, payload, payload_len, client->priority)) {
            return XGW_RING_FRAME_BACKPRESSURE; /* 发送队列暂满，本轮停止、下轮重读 */
        }
        bridge_client_note_first_packet(client, data_len, "shared_ring");
        return XGW_RING_FRAME_OK;
    }
    if (kind == XGW_BRIDGE_RING_KIND_CLOSE) {
        uint8_t payload[XGW_BRIDGE_WIRE_PAYLOAD_MAX];
        size_t payload_len = encode_close_payload(client->stream_id, payload, sizeof(payload));
        if (payload_len > 0U) {
            (void) bridge_pending_push_ex(server, payload, payload_len, client->priority);
        }
        close_client_with_reason(client, "front_close");
        return 1;
    }
    if (kind == XGW_BRIDGE_RING_KIND_HALF_CLOSE) {
        uint8_t payload[XGW_BRIDGE_WIRE_PAYLOAD_MAX];
        size_t payload_len = encode_half_close_payload(client->stream_id, payload, sizeof(payload));
        if (payload_len > 0U) {
            (void) bridge_pending_push_ex(server, payload, payload_len, client->priority);
        }
        mark_client_front_eof(client);
        return 1;
    }
    return 1;
}

static int poll_ring_frames(xgw_bridge_server_t *server) {
    uint8_t header[XGW_BRIDGE_RING_HEADER_SIZE];
    uint64_t write_seq = 0U;
    uint64_t read_seq = 0U;
    unsigned int handled = 0U;
    if (server == NULL) {
        return 1;
    }
    if (!ring_read_header(&server->ring_rx, header, &write_seq, &read_seq)) {
        printf("bridge.ring.poll.header_fail\n");
        fflush(stdout);
        return 0;
    }
    /* Generation bump = peer republished a new epoch. Reattach in place (mmap
     * preserved) and re-read the header to pick up the epoch's reset seqs; do
     * NOT reopen. Reopen/structural recovery is owned by ring_read_header's
     * magic/version/capacity rejection above. The poll loop only ever inspects
     * the header, never the file's lifecycle. */
    if (ring_generation_changed(&server->ring_rx)) {
        if (XGW_BRIDGE_VERBOSE()) {
            printf("bridge.ring.poll.reattach path=%s\n", server->ring_rx.path);
            fflush(stdout);
        }
        ring_attach(&server->ring_rx);
        if (!ring_read_header(&server->ring_rx, header, &write_seq, &read_seq)) {
            return 0;
        }
    } else {
        (void) ring_ensure_attached(&server->ring_rx);
    }
    if (XGW_BRIDGE_VERBOSE()) {
        printf("bridge.ring.poll.header write_seq=%llu read_seq=%llu capacity=%u slot_size=%u\n",
               (unsigned long long) write_seq,
               (unsigned long long) read_seq,
               server->ring_rx.capacity,
               server->ring_rx.slot_size);
        fflush(stdout);
    }
    if (!ring_peer_ready(&server->ring_rx)) {
        if (XGW_BRIDGE_VERBOSE()) {
            printf("bridge.ring.poll.peer_not_ready path=%s flags=%u generation=%llu\n",
                   server->ring_rx.path,
                   (unsigned int) server->ring_rx.flags,
                   (unsigned long long) server->ring_rx.generation);
            fflush(stdout);
        }
    }
    while (handled < XGW_BRIDGE_RING_BATCH_LIMIT && read_seq < write_seq) {
        uint8_t *slot;
        const uint8_t *frame;
        uint32_t frame_len;
        uint64_t slot_seq;
        long offset = ring_slot_offset(read_seq, server->ring_rx.capacity, server->ring_rx.slot_size);
        if (offset < 0 || (size_t) offset + XGW_BRIDGE_RING_SLOT_HEAD > server->ring_rx.map_len) {
            printf("bridge.ring.poll.offset_invalid read_seq=%llu offset=%ld map_len=%zu\n",
                   (unsigned long long) read_seq,
                   offset,
                   server->ring_rx.map_len);
            fflush(stdout);
            return 0;
        }
        slot = server->ring_rx.map + offset;
        slot_seq = read_le64(slot);
        if (slot_seq != read_seq + 1U) {
            if (XGW_BRIDGE_VERBOSE()) {
                printf("bridge.ring.poll.slot_wait read_seq=%llu expect=%llu got=%llu offset=%ld\n",
                       (unsigned long long) read_seq,
                       (unsigned long long) (read_seq + 1U),
                       (unsigned long long) slot_seq,
                       offset);
                fflush(stdout);
            }
            break;
        }
        frame_len = read_le32(slot + 8U);
        if (frame_len > XGW_BRIDGE_RING_FRAME_MAX ||
            (size_t) offset + XGW_BRIDGE_RING_SLOT_HEAD + (size_t) frame_len > server->ring_rx.map_len) {
            printf("bridge.ring.poll.frame_invalid read_seq=%llu frame_len=%u frame_max=%u offset=%ld map_len=%zu\n",
                   (unsigned long long) read_seq,
                   frame_len,
                   (unsigned int) XGW_BRIDGE_RING_FRAME_MAX,
                   offset,
                   server->ring_rx.map_len);
            fflush(stdout);
            return 0;
        }
        frame = slot + XGW_BRIDGE_RING_SLOT_HEAD;
        if (XGW_BRIDGE_VERBOSE()) {
            printf("bridge.ring.poll.frame read_seq=%llu frame_len=%u kind=%u client=%u\n",
                   (unsigned long long) read_seq,
                   frame_len,
                   (unsigned int) frame[0],
                   (unsigned int) read_be32(frame + 1U));
            fflush(stdout);
        }
        {
            int frame_rc = handle_ring_frame(server, frame, frame_len);
            if (frame_rc == XGW_RING_FRAME_BACKPRESSURE) {
                /* 发送队列暂满：停止本轮 drain，read_seq 不前进 → 下轮重读此帧。
                 * 绝不杀进程（这是正常背压，非结构损坏）。已处理的帧在循环末统一回写。 */
                if (XGW_BRIDGE_VERBOSE()) {
                    printf("bridge.ring.poll.backpressure read_seq=%llu pending_count=%zu\n",
                           (unsigned long long) read_seq,
                           server->pending_count);
                    fflush(stdout);
                }
                break;
            }
            if (frame_rc == XGW_RING_FRAME_ERR) {
                printf("bridge.ring.poll.handle_fail read_seq=%llu frame_len=%u\n",
                       (unsigned long long) read_seq,
                       frame_len);
                fflush(stdout);
                return 0;
            }
        }
        read_seq++;
        handled++;
    }
    if (handled > 0U) {
        write_le64(server->ring_rx.map + XGW_BRIDGE_RING_READ_SEQ_OFFSET, read_seq);
        ring_bump_notify(&server->ring_rx);
    }
    return 1;
}

static int accept_new_client(xgw_bridge_server_t *server) {
    int client_fd = accept(server->listen_fd, NULL, NULL);
    xgw_bridge_client_t *client;
    if (client_fd < 0) {
        return 1;
    }
    if (server->listen_transport == XGW_BRIDGE_TRANSPORT_TCP) {
        xgw_set_tcp_nodelay(client_fd);
    }
    client = allocate_client(server);
    if (client == NULL) {
        (void) write_status(client_fd, 0U, "bridge client table full");
        xgw_close_socket(client_fd);
        return 1;
    }
    client->fd = client_fd;
    if (!recv_request_ex(client_fd,
                         &client->proto,
                         client->target_host,
                         sizeof(client->target_host),
                         &client->target_port,
                         &client->meta,
                         &client->session_meta)) {
        (void) write_status(client_fd, 0U, "invalid bridge request");
        close_client(client);
        return 1;
    }
    client->priority = client->meta.priority == 0U
                       ? bridge_classify_priority(client->target_host, client->target_port)
                       : (uint32_t) client->meta.priority;
    if (!write_status(client_fd, 1U, "queued")) {
        close_client_with_reason(client, "bridge_status_write_fail");
        return 1;
    }
    bridge_client_log_accept(client);
    printf("bridge.accept stream=%u proto=%s target=%s:%u priority=%u flow_class=%u budget_flags=%u copies=%u\n",
           client->stream_id,
           client->proto == XGW_BRIDGE_PROTO_UDP ? "udp" : "tcp",
           client->target_host,
           client->target_port,
           client->priority,
           (unsigned int) client->meta.flow_class,
           (unsigned int) client->meta.budget_flags,
           (unsigned int) client->meta.preferred_copies);
    fflush(stdout);
    return 1;
}

int xgw_bridge_server_start(xgw_bridge_server_t **out_server,
                            const xgw_runtime_config_t *config,
                            char *error,
                            size_t error_len) {
    xgw_bridge_server_t *server = NULL;
    int fd;
    if (out_server == NULL || config == NULL) {
        set_error(error, error_len, "invalid bridge args");
        return 0;
    }
    if (strcmp(config->connect_type, "bridge") != 0) {
        *out_server = NULL;
        return 1;
    }
    if (!xgw_socket_runtime_init()) {
        set_error(error, error_len, "socket runtime init failed");
        return 0;
    }
    server = (xgw_bridge_server_t *) calloc(1, sizeof(*server));
    if (server == NULL) {
        set_error(error, error_len, "bridge alloc failed");
        return 0;
    }
    server->config = *config;
    server->next_stream_id = 1000U;
    server->listen_fd = -1;
    if (strcmp(config->bridge_transport, "unix") == 0) {
#ifdef _WIN32
        free(server);
        set_error(error, error_len, "bridge_transport=unix is not supported on Windows");
        return 0;
#else
        struct sockaddr_un addr;
        if (config->bridge_unix_listen[0] == '\0') {
            free(server);
            set_error(error, error_len, "connect_type=bridge requires bridge_unix_listen");
            return 0;
        }
        if (strlen(config->bridge_unix_listen) >= sizeof(addr.sun_path)) {
            free(server);
            set_error(error, error_len, "bridge_unix_listen path too long");
            return 0;
        }
        fd = (int) socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == XGW_INVALID_SOCKET) {
            free(server);
            set_error(error, error_len, "bridge unix socket failed");
            return 0;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", config->bridge_unix_listen);
        (void) unlink(config->bridge_unix_listen);
        if (bind(fd, (struct sockaddr *) &addr, (socklen_arg_t) sizeof(addr)) == SOCKET_ERROR) {
            xgw_close_socket(fd);
            free(server);
            set_error(error, error_len, "bridge unix bind failed");
            return 0;
        }
        if (listen(fd, 64) == SOCKET_ERROR) {
            xgw_close_socket(fd);
            (void) unlink(config->bridge_unix_listen);
            free(server);
            set_error(error, error_len, "bridge unix listen failed");
            return 0;
        }
        server->listen_transport = XGW_BRIDGE_TRANSPORT_UNIX;
#endif
    } else if (strcmp(config->bridge_transport, "shared-ring") == 0 ||
               strcmp(config->bridge_transport, "shared_ring") == 0 ||
               strcmp(config->bridge_transport, "ring") == 0 ||
               strcmp(config->bridge_transport, "shm-direct") == 0 ||
               strcmp(config->bridge_transport, "shm_direct") == 0 ||
               strcmp(config->bridge_transport, "shared-memory-direct") == 0) {
        char go2c[256];
        char c2go[256];
        if (config->bridge_ring_path[0] == '\0') {
            free(server);
            set_error(error, error_len, "connect_type=bridge requires bridge_ring_path");
            return 0;
        }
        snprintf(go2c, sizeof(go2c), "%s.go2c", config->bridge_ring_path);
        snprintf(c2go, sizeof(c2go), "%s.c2go", config->bridge_ring_path);
        if (!ring_init_file(&server->ring_rx, go2c) ||
            !ring_init_file(&server->ring_tx, c2go)) {
            ring_close_file(&server->ring_rx);
            ring_close_file(&server->ring_tx);
            free(server);
            set_error(error, error_len, "bridge shared-ring init failed");
            return 0;
        }
        server->listen_transport = XGW_BRIDGE_TRANSPORT_RING;
        fd = -1;
    } else if (strcmp(config->bridge_transport, "tcp") == 0 || config->bridge_transport[0] == '\0') {
        struct sockaddr_in addr;
        char host[64];
        uint16_t port = 0U;
        int one = 1;
        if (config->bridge_tcp_listen[0] == '\0') {
            free(server);
            set_error(error, error_len, "connect_type=bridge requires bridge_tcp_listen");
            return 0;
        }
        if (!parse_host_port_text(config->bridge_tcp_listen, host, sizeof(host), &port)) {
            free(server);
            set_error(error, error_len, "invalid bridge_tcp_listen");
            return 0;
        }
        fd = (int) socket(AF_INET, SOCK_STREAM, 0);
        if (fd == XGW_INVALID_SOCKET) {
            free(server);
            set_error(error, error_len, "bridge socket failed");
            return 0;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (strcmp(host, "0.0.0.0") == 0) {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            xgw_close_socket(fd);
            free(server);
            set_error(error, error_len, "bridge listen host invalid");
            return 0;
        }
        if (bind(fd, (struct sockaddr *) &addr, (socklen_arg_t) sizeof(addr)) == SOCKET_ERROR) {
            xgw_close_socket(fd);
            free(server);
            set_error(error, error_len, "bridge bind failed");
            return 0;
        }
        if (listen(fd, 64) == SOCKET_ERROR) {
            xgw_close_socket(fd);
            free(server);
            set_error(error, error_len, "bridge listen failed");
            return 0;
        }
        server->listen_transport = XGW_BRIDGE_TRANSPORT_TCP;
    } else {
        free(server);
        set_error(error, error_len, "unsupported bridge_transport");
        return 0;
    }
    server->listen_fd = fd;
    *out_server = server;
    printf("bridge.start transport=%s listen=%s\n",
           config->bridge_transport,
           server->listen_transport == XGW_BRIDGE_TRANSPORT_RING
               ? config->bridge_ring_path
               : (server->listen_transport == XGW_BRIDGE_TRANSPORT_UNIX ? config->bridge_unix_listen : config->bridge_tcp_listen));
    fflush(stdout);
    return 1;
}

int xgw_bridge_server_poll(xgw_bridge_server_t *server, int timeout_ms) {
    fd_set rfds;
    struct timeval tv;
    int ready;
    if (server == NULL) {
        return 1;
    }
    if (server->listen_transport == XGW_BRIDGE_TRANSPORT_RING) {
        (void) timeout_ms;
        return poll_ring_frames(server);
    }
    if (server->listen_fd < 0) {
        return 1;
    }
    FD_ZERO(&rfds);
    FD_SET((unsigned int) server->listen_fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ready = select(server->listen_fd + 1, &rfds, NULL, NULL, timeout_ms >= 0 ? &tv : NULL);
    if (ready <= 0) {
        return 1;
    }
    if (FD_ISSET((unsigned int) server->listen_fd, &rfds)) {
        return accept_new_client(server);
    }
    return 1;
}

int xgw_bridge_server_dequeue(xgw_bridge_server_t *server,
                              uint8_t *buf,
                              size_t buf_cap,
                              size_t *out_len,
                              char *error,
                              size_t error_len) {
    size_t i;
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (server == NULL) {
        return 0;
    }
    {
        int pending_rc = bridge_pending_pop(server, buf, buf_cap, out_len);
        if (pending_rc < 0) {
            set_error(error, error_len, "bridge pending payload too large");
            return -1;
        }
        if (pending_rc > 0) {
            return 1;
        }
    }
    for (i = 0; i < XGW_BRIDGE_MAX_CLIENTS; ++i) {
        xgw_bridge_client_t *client = &server->clients[i];
        if (!client->active) {
            continue;
        }
        if (client->is_ring) {
            continue;
        }
        if (!client->open_sent) {
            size_t n = encode_open_payload(client, buf, buf_cap);
            if (n == 0U) {
                set_error(error, error_len, "bridge open payload too large");
                return -1;
            }
            client->open_sent = 1;
            if (out_len != NULL) {
                *out_len = n;
            }
            return 1;
        }
        if (client->proto == XGW_BRIDGE_PROTO_UDP && client->udp_rx_len > 0U) {
            int frame_rc = decode_udp_front_frame(client, buf, buf_cap, out_len);
            if (frame_rc < 0) {
                size_t n = encode_close_payload(client->stream_id, buf, buf_cap);
                close_client_with_reason(client, "front_udp_frame_invalid");
                if (n == 0U) {
                    set_error(error, error_len, "bridge udp close payload too large");
                    return -1;
                }
                if (out_len != NULL) {
                    *out_len = n;
                }
                return 1;
            }
            if (frame_rc > 0) {
                bridge_client_note_first_packet(client, out_len != NULL ? *out_len : 0U, "front_udp");
                return 1;
            }
        }
        {
            fd_set rfds;
            struct timeval tv;
            uint8_t read_buf[XGW_BRIDGE_MAX_PAYLOAD];
            int ready;
            FD_ZERO(&rfds);
            FD_SET((unsigned int) client->fd, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            ready = select(client->fd + 1, &rfds, NULL, NULL, &tv);
            if (ready > 0 && FD_ISSET((unsigned int) client->fd, &rfds)) {
                int nread;
                if (client->proto == XGW_BRIDGE_PROTO_UDP) {
                    size_t free_cap = sizeof(client->udp_rx_buf) - client->udp_rx_len;
                    if (free_cap == 0U) {
                        size_t n = encode_close_payload(client->stream_id, buf, buf_cap);
                        close_client_with_reason(client, "front_udp_rx_overflow");
                        if (n == 0U) {
                            set_error(error, error_len, "bridge udp close payload too large");
                            return -1;
                        }
                        if (out_len != NULL) {
                            *out_len = n;
                        }
                        return 1;
                    }
                    nread = recv(client->fd,
                                 (char *) (client->udp_rx_buf + client->udp_rx_len),
                                 (int) free_cap,
                                 0);
                } else {
                    nread = recv(client->fd, (char *) read_buf, (int) sizeof(read_buf), 0);
                }
                if (nread <= 0) {
                    if (client->proto == XGW_BRIDGE_PROTO_TCP && nread == 0) {
                        (void) mark_client_front_eof(client);
                        continue;
                    } else {
                        size_t n = encode_close_payload(client->stream_id, buf, buf_cap);
                        client->close_sent = 1;
                        close_client_with_reason(client, nread == 0 ? "front_recv_eof" : "front_recv_fail");
                        if (n == 0U) {
                            set_error(error, error_len, "bridge close payload too large");
                            return -1;
                        }
                        if (out_len != NULL) {
                            *out_len = n;
                        }
                        return 1;
                    }
                }
                if (client->proto == XGW_BRIDGE_PROTO_UDP) {
                    int frame_rc;
                    client->udp_rx_len += (size_t) nread;
                    bridge_client_touch_activity(client);
                    frame_rc = decode_udp_front_frame(client, buf, buf_cap, out_len);
                    if (frame_rc < 0) {
                        size_t n = encode_close_payload(client->stream_id, buf, buf_cap);
                        close_client_with_reason(client, "front_udp_frame_invalid");
                        if (n == 0U) {
                            set_error(error, error_len, "bridge udp close payload too large");
                            return -1;
                        }
                        if (out_len != NULL) {
                            *out_len = n;
                        }
                        return 1;
                    }
                    if (frame_rc > 0) {
                        bridge_client_note_first_packet(client, out_len != NULL ? *out_len : 0U, "front_udp");
                        return 1;
                    }
                    continue;
                }
                {
                    size_t n = 0U;
                    bridge_client_touch_activity(client);
                    n = encode_data_payload(client->stream_id, read_buf, (size_t) nread, buf, buf_cap);
                    if (n == 0U) {
                        set_error(error, error_len, "bridge data payload too large");
                        return -1;
                    }
                    if (out_len != NULL) {
                        *out_len = n;
                    }
                    bridge_client_note_first_packet(client, (size_t) nread, "front_tcp");
                    return 1;
                }
            }
        }
    }
    return 0;
}

int xgw_bridge_server_handle_payload(xgw_bridge_server_t *server,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     char *error,
                                     size_t error_len) {
    uint32_t magic;
    uint32_t stream_id;
    uint8_t kind;
    xgw_bridge_client_t *client;
    if (server == NULL || payload == NULL || payload_len < 9U) {
        return 0;
    }
    magic = read_be32(payload);
    if (magic != XGW_BRIDGE_MAGIC) {
        return 0;
    }
    stream_id = read_be32(payload + 4U);
    kind = payload[8];
    client = find_client_by_stream(server, stream_id);
    if (client == NULL) {
        return 0;
    }
    if (kind == XGW_BRIDGE_KIND_TCP_DATA) {
        uint16_t data_len;
        if (payload_len < 11U) {
            set_error(error, error_len, "bridge data payload too short");
            return -1;
        }
        data_len = read_be16(payload + 9U);
        if (payload_len < 11U + data_len) {
            set_error(error, error_len, "bridge data payload truncated");
            return -1;
        }
        if (client->is_ring) {
            if (!ring_write_frame(server, XGW_BRIDGE_RING_KIND_TCP_DATA, client->ring_client_id, payload + 11U, data_len)) {
                close_client_with_reason(client, "front_shared_ring_write_fail");
            } else {
                bridge_client_touch_activity(client);
                bridge_client_note_first_byte(client, data_len, "xgw_return");
                bridge_client_note_return_chunk(client, data_len, "xgw_return");
                /* 回程时间线埋点:ingress 把回程数据写入 c2go ring 交给前端的时刻(第三点)。
                 * 仅首字节打一次,避免刷屏(note_first_byte 内部已判重,这里用 return_bytes 判)。 */
                if (client->return_bytes == data_len) {
                    log4c_info("rtt.ingress.return_to_front stream=%u bytes=%u", stream_id, (unsigned int) data_len);
                }
            }
        } else if (!send_all(client->fd, payload + 11U, data_len)) {
            close_client_with_reason(client, "front_send_fail");
        } else {
            bridge_client_touch_activity(client);
            bridge_client_note_first_byte(client, data_len, "xgw_return");
            bridge_client_note_return_chunk(client, data_len, "xgw_return");
        }
        if (XGW_BRIDGE_VERBOSE()) printf("bridge.ingress.deliver stream=%u len=%u transport=%s\n",
               stream_id,
               data_len,
               client->is_ring ? "shared-ring" : "socket");
        fflush(stdout);
        return 1;
    }
    if (kind == XGW_BRIDGE_KIND_UDP_DATA) {
        uint16_t addr_len;
        uint16_t data_len;
        const uint8_t *data;
        if (payload_len < 13U) {
            set_error(error, error_len, "bridge udp payload too short");
            return -1;
        }
        addr_len = read_be16(payload + 9U);
        if (addr_len == 0U || payload_len < 13U + addr_len) {
            set_error(error, error_len, "bridge udp payload truncated");
            return -1;
        }
        data_len = read_be16(payload + 11U + addr_len);
        if (payload_len < 13U + addr_len + data_len) {
            set_error(error, error_len, "bridge udp data truncated");
            return -1;
        }
        data = payload + 13U + addr_len;
        if (client->is_ring) {
            uint8_t body[XGW_BRIDGE_RING_FRAME_MAX];
            size_t body_len = 4U + addr_len + data_len;
            if (body_len > sizeof(body)) {
                set_error(error, error_len, "bridge udp ring payload too large");
                return -1;
            }
            write_be16(body, addr_len);
            memcpy(body + 2U, payload + 11U, addr_len);
            write_be16(body + 2U + addr_len, data_len);
            memcpy(body + 4U + addr_len, data, data_len);
            if (!ring_write_frame(server, XGW_BRIDGE_RING_KIND_UDP_DATA, client->ring_client_id, body, body_len)) {
                close_client_with_reason(client, "front_shared_ring_write_fail");
            } else {
                bridge_client_touch_activity(client);
                bridge_client_note_first_byte(client, data_len, "xgw_return");
                bridge_client_note_return_chunk(client, data_len, "xgw_return");
            }
        } else {
            uint8_t head[4];
            write_be16(head, addr_len);
            write_be16(head + 2U, data_len);
            if (!send_all(client->fd, head, 2U) ||
                !send_all(client->fd, payload + 11U, addr_len) ||
                !send_all(client->fd, head + 2U, 2U) ||
                !send_all(client->fd, data, data_len)) {
                close_client_with_reason(client, "front_send_fail");
            } else {
                bridge_client_touch_activity(client);
                bridge_client_note_first_byte(client, data_len, "xgw_return");
                bridge_client_note_return_chunk(client, data_len, "xgw_return");
            }
        }
        if (XGW_BRIDGE_VERBOSE()) printf("bridge.ingress.deliver_udp stream=%u addr_len=%u len=%u transport=%s\n",
               stream_id,
               addr_len,
               data_len,
               client->is_ring ? "shared-ring" : "socket");
        fflush(stdout);
        return 1;
    }
    if (kind == XGW_BRIDGE_KIND_TCP_CLOSE) {
        printf("bridge.ingress.close stream=%u\n", stream_id);
        fflush(stdout);
        client->close_sent = 1;
        if (client->is_ring) {
            (void) ring_write_frame(server, XGW_BRIDGE_RING_KIND_CLOSE, client->ring_client_id, NULL, 0U);
        }
        close_client_with_reason(client, "upstream_close");
        return 1;
    }
    if (kind == XGW_BRIDGE_KIND_TCP_HALF_CLOSE) {
        mark_client_front_eof(client);
        return 1;
    }
    return 0;
}

void xgw_bridge_server_stop(xgw_bridge_server_t *server) {
    size_t i;
    if (server == NULL) {
        return;
    }
    for (i = 0; i < XGW_BRIDGE_MAX_CLIENTS; ++i) {
        close_client(&server->clients[i]);
    }
    if (server->listen_fd >= 0) {
        xgw_close_socket(server->listen_fd);
    }
    if (server->listen_transport == XGW_BRIDGE_TRANSPORT_RING) {
        ring_close_file(&server->ring_rx);
        ring_close_file(&server->ring_tx);
    }
#ifndef _WIN32
    if (server->listen_transport == XGW_BRIDGE_TRANSPORT_UNIX &&
        server->config.bridge_unix_listen[0] != '\0') {
        (void) unlink(server->config.bridge_unix_listen);
    }
#endif
    free(server);
}

static xgw_bridge_egress_session_t *find_egress_session(xgw_bridge_egress_t *bridge, uint32_t stream_id) {
    size_t i;
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        if (bridge->sessions[i].active && bridge->sessions[i].stream_id == stream_id) {
            return &bridge->sessions[i];
        }
    }
    return NULL;
}

static xgw_bridge_egress_session_t *alloc_egress_session(xgw_bridge_egress_t *bridge, uint32_t stream_id) {
    size_t i;
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        if (!bridge->sessions[i].active) {
            memset(&bridge->sessions[i], 0, sizeof(bridge->sessions[i]));
            bridge->sessions[i].active = 1;
            bridge->sessions[i].fd = -1;
            bridge->sessions[i].stream_id = stream_id;
            bridge->sessions[i].open_started_us = xgw_bridge_now_us();
            bridge->sessions[i].priority = XGW_BRIDGE_PRIORITY_NORMAL;
            return &bridge->sessions[i];
        }
    }
    return NULL;
}

static void close_egress_session(xgw_bridge_egress_session_t *session) {
    if (session == NULL || !session->active) {
        return;
    }
    egress_session_log_close(session);
    if (session->fd >= 0) {
        xgw_close_socket(session->fd);
    }
    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

static int egress_queue_push_ex(xgw_bridge_egress_session_t *session, const char *addr, const uint8_t *data, size_t len) {
    size_t idx;
    if (session == NULL || data == NULL || len == 0U || len > XGW_BRIDGE_MAX_PAYLOAD) {
        return 0;
    }
    if (session->pending_count >= xgw_bridge_pending_limit()) {
        return 0;
    }
    idx = (session->pending_head + session->pending_count) % XGW_BRIDGE_PENDING_CHUNKS;
    memcpy(session->pending_data[idx], data, len);
    snprintf(session->pending_addr[idx], sizeof(session->pending_addr[idx]), "%s", addr == NULL ? "" : addr);
    session->pending_lengths[idx] = len;
    session->pending_count++;
    return 1;
}

static int egress_queue_pop_ex(xgw_bridge_egress_session_t *session,
                               char *addr,
                               size_t addr_cap,
                               uint8_t *buf,
                               size_t buf_cap,
                               size_t *out_len) {
    size_t idx;
    size_t len;
    if (session == NULL || buf == NULL || out_len == NULL || session->pending_count == 0U) {
        return 0;
    }
    idx = session->pending_head;
    len = session->pending_lengths[idx];
    if (len > buf_cap) {
        return 0;
    }
    if (addr != NULL && addr_cap > 0U) {
        snprintf(addr, addr_cap, "%s", session->pending_addr[idx]);
    }
    memcpy(buf, session->pending_data[idx], len);
    *out_len = len;
    session->pending_head = (session->pending_head + 1U) % XGW_BRIDGE_PENDING_CHUNKS;
    session->pending_count--;
    return 1;
}

static void bridge_egress_drain_socket(xgw_bridge_egress_session_t *session) {
    while (session != NULL && session->fd >= 0 && !session->pending_close) {
        uint8_t read_buf[XGW_BRIDGE_MAX_PAYLOAD];
        struct sockaddr_storage from;
        socklen_arg_t from_len = (socklen_arg_t) sizeof(from);
        char from_text[256];
        const char *push_addr = NULL;
        int nread;
        memset(&from, 0, sizeof(from));
        if (session->is_udp) {
            nread = recvfrom(session->fd, (char *) read_buf, (int) sizeof(read_buf), 0,
                             (struct sockaddr *) &from, &from_len);
        } else {
            nread = recv(session->fd, (char *) read_buf, (int) sizeof(read_buf), 0);
        }
        if (nread < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                break;
            }
            /* unconnected UDP：前一次 sendto 触发的 ICMP 不可达会让 recvfrom
             * 返回 WSAECONNRESET，这不是会话级致命错误，跳过即可。 */
            if (session->is_udp && (e == WSAECONNRESET || e == WSAENETRESET)) {
                break;
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            /* unconnected UDP：ICMP 不可达极少会被投递为 recvfrom 错误，
             * 但若出现 ECONNREFUSED 同样按非致命处理。 */
            if (session->is_udp && errno == ECONNREFUSED) {
                break;
            }
#endif
            egress_session_set_close_reason(session, "target_recv_fail");
            session->pending_close = 1;
            xgw_close_socket(session->fd);
            session->fd = -1;
            printf("bridge.egress.recv.fail stream=%u target=%s:%u\n",
                   session->stream_id,
                   session->target_host,
                   session->target_port);
            fflush(stdout);
            break;
        }
        if (nread == 0) {
            if (session->is_udp) {
                /* 合法的 0 长度 UDP datagram，不能当作 EOF 关流；丢弃并继续。 */
                continue;
            }
            egress_session_set_close_reason(session, "target_recv_eof");
            session->pending_close = 1;
            xgw_close_socket(session->fd);
            session->fd = -1;
            break;
        }
        if (session->is_udp) {
            if (bridge_sockaddr_to_text(&from, (socklen_t) from_len, from_text, sizeof(from_text))) {
                push_addr = from_text;
            } else {
                /* 来源地址无法格式化时回退到首目标，保证回传仍可编码。
                 * host 宽度受限避免极端长主机名溢出。 */
                snprintf(from_text, sizeof(from_text), "%.240s:%u",
                         session->target_host, session->target_port);
                push_addr = from_text;
            }
        }
        if (!egress_queue_push_ex(session,
                                  push_addr,
                                  read_buf,
                                  (size_t) nread)) {
            printf("bridge.egress.queue.full stream=%u target=%s:%u len=%d\n",
                   session->stream_id,
                   session->target_host,
                   session->target_port,
                   nread);
            fflush(stdout);
            egress_session_set_close_reason(session, "return_queue_full");
            session->pending_close = 1;
            xgw_close_socket(session->fd);
            session->fd = -1;
            break;
        }
        egress_session_touch_activity(session);
        egress_session_note_first_recv(session, (size_t) nread);
        egress_session_note_recv_chunk(session, (size_t) nread);
        if (XGW_BRIDGE_VERBOSE()) printf("bridge.egress.recv stream=%u proto=%s target=%s:%u len=%d\n",
               session->stream_id,
               session->is_udp ? "udp" : "tcp",
               session->target_host,
               session->target_port,
               nread);
        fflush(stdout);
    }
}

int xgw_bridge_egress_init(xgw_bridge_egress_t **out_bridge, char *error, size_t error_len) {
    xgw_bridge_egress_t *bridge;
    if (out_bridge == NULL) {
        set_error(error, error_len, "invalid egress bridge args");
        return 0;
    }
    if (!xgw_socket_runtime_init()) {
        set_error(error, error_len, "socket runtime init failed");
        return 0;
    }
    bridge = (xgw_bridge_egress_t *) calloc(1, sizeof(*bridge));
    if (bridge == NULL) {
        set_error(error, error_len, "egress bridge alloc failed");
        return 0;
    }
    *out_bridge = bridge;
    return 1;
}

int xgw_bridge_egress_handle_payload(xgw_bridge_egress_t *bridge,
                                     const char *upstream_host,
                                     uint16_t upstream_port,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     char *error,
                                     size_t error_len) {
    uint32_t magic;
    uint32_t stream_id;
    uint8_t kind;
    xgw_bridge_egress_session_t *session;
    if (bridge == NULL || payload == NULL || payload_len < 9U) {
        return 0;
    }
    if (upstream_host != NULL && upstream_host[0] != '\0' && upstream_port != 0U) {
        snprintf(bridge->upstream_host, sizeof(bridge->upstream_host), "%s", upstream_host);
        bridge->upstream_port = upstream_port;
        snprintf(bridge->last_upstream_host, sizeof(bridge->last_upstream_host), "%s", upstream_host);
        bridge->last_upstream_port = upstream_port;
    }
    magic = read_be32(payload);
    if (magic != XGW_BRIDGE_MAGIC) {
        return 0;
    }
    stream_id = read_be32(payload + 4U);
    kind = payload[8];
    printf("bridge.egress.handle stream=%u kind=%u payload_len=%zu upstream=%s:%u\n",
           stream_id,
           (unsigned int) kind,
           payload_len,
           upstream_host == NULL ? "" : upstream_host,
           upstream_port);
    fflush(stdout);
    if (kind == XGW_BRIDGE_KIND_TCP_OPEN || kind == XGW_BRIDGE_KIND_UDP_OPEN) {
        uint16_t host_len;
        char host[256];
        uint16_t port;
        const uint8_t *meta = NULL;
        size_t meta_len = 0U;
        if (payload_len < 13U) {
            set_error(error, error_len, "bridge open payload too short");
            return -1;
        }
        host_len = read_be16(payload + 9U);
        if (payload_len < 13U + host_len || host_len >= sizeof(host)) {
            set_error(error, error_len, "bridge open payload truncated");
            return -1;
        }
        memcpy(host, payload + 11U, host_len);
        host[host_len] = '\0';
        port = read_be16(payload + 11U + host_len);
        if (payload_len >= 13U + host_len + XGW_BRIDGE_META_LEN_V1) {
            meta = payload + 13U + host_len;
            meta_len = payload_len - (13U + host_len);
            if (meta_len > XGW_BRIDGE_META_LEN) {
                meta_len = XGW_BRIDGE_META_LEN;
            }
        }
        printf("bridge.egress.handle.open stream=%u proto=%s target=%s:%u host_len=%u\n",
               stream_id,
               kind == XGW_BRIDGE_KIND_UDP_OPEN ? "udp" : "tcp",
               host,
               port,
               (unsigned int) host_len);
        fflush(stdout);
        session = alloc_egress_session(bridge, stream_id);
        if (session == NULL) {
            (void) error;
            (void) error_len;
            printf("bridge.egress.alloc.fail stream=%u target=%s:%u\n", stream_id, host, port);
            fflush(stdout);
            return 1;
        }
        snprintf(session->target_host, sizeof(session->target_host), "%s", host);
        session->target_port = port;
        session->priority = bridge_classify_priority(host, port);
        bridge_default_meta(&session->meta, session->priority);
        if (meta != NULL && meta_len >= XGW_BRIDGE_META_LEN_V1) {
            decode_bridge_meta_compat(meta, meta_len, &session->meta);
            if (session->meta.priority > 0U) {
                session->priority = session->meta.priority;
            }
        }
        snprintf(session->upstream_host, sizeof(session->upstream_host), "%s", upstream_host == NULL ? "" : upstream_host);
        session->upstream_port = upstream_port;
        session->is_udp = (kind == XGW_BRIDGE_KIND_UDP_OPEN);
        session->open_started_us = xgw_bridge_now_us();
        session->fd = session->is_udp ? udp_open_unbound(host, port) : tcp_connect_ipv4(host, port);
        if (session->fd < 0) {
            egress_session_set_close_reason(session, "target_connect_fail");
            session->pending_close = 1;
            printf("bridge.egress.open.fail stream=%u proto=%s target=%s:%u\n",
                   stream_id,
                   session->is_udp ? "udp" : "tcp",
                   host,
                   port);
            fflush(stdout);
            return 1;
        }
        session->connected_us = xgw_bridge_now_us();
        egress_session_log_open(session);
        printf("bridge.egress.open stream=%u proto=%s target=%s:%u local_port=%u\n",
               stream_id,
               session->is_udp ? "udp" : "tcp",
               host,
               port,
               socket_local_port(session->fd));
        fflush(stdout);
        if (session->is_udp) {
            bridge_egress_drain_socket(session);
        }
        return 1;
    }
    session = find_egress_session(bridge, stream_id);
    if (session == NULL) {
        printf("bridge.egress.handle.miss stream=%u kind=%u payload_len=%zu upstream=%s:%u\n",
               stream_id,
               (unsigned int) kind,
               payload_len,
               upstream_host == NULL ? "" : upstream_host,
               upstream_port);
        fflush(stdout);
        return 0;
    }
    if (session->pending_close || session->fd < 0) {
        printf("bridge.egress.handle.skip stream=%u kind=%u pending_close=%d fd=%d target=%s:%u\n",
               stream_id,
               (unsigned int) kind,
               session->pending_close,
               session->fd,
               session->target_host,
               session->target_port);
        fflush(stdout);
        return 1;
    }
    if (kind == XGW_BRIDGE_KIND_TCP_DATA) {
        uint16_t data_len;
        if (payload_len < 11U) {
            printf("bridge.egress.data.short stream=%u len=%zu\n", stream_id, payload_len);
            fflush(stdout);
            session->pending_close = 1;
            return 1;
        }
        data_len = read_be16(payload + 9U);
        if (payload_len < 11U + data_len) {
            printf("bridge.egress.data.truncated stream=%u len=%zu want=%u\n", stream_id, payload_len, data_len);
            fflush(stdout);
            session->pending_close = 1;
            return 1;
        }
        if (!send_all(session->fd, payload + 11U, data_len)) {
            egress_session_set_close_reason(session, "target_send_fail");
            session->pending_close = 1;
            xgw_close_socket(session->fd);
            session->fd = -1;
        printf("bridge.egress.send.fail stream=%u target=%s:%u len=%u\n",
               session->stream_id,
               session->target_host,
               session->target_port,
               data_len);
            fflush(stdout);
            return 1;
        }
        egress_session_note_first_send(session, data_len);
        egress_session_touch_activity(session);
        egress_session_note_send_chunk(session, data_len);
        if (XGW_BRIDGE_VERBOSE()) printf("bridge.egress.send stream=%u target=%s:%u len=%u\n",
               session->stream_id,
               session->target_host,
               session->target_port,
               data_len);
        fflush(stdout);
        bridge_egress_drain_socket(session);
        return 1;
    }
    if (kind == XGW_BRIDGE_KIND_UDP_DATA) {
        uint16_t addr_len;
        uint16_t data_len;
        if (payload_len < 13U) {
            printf("bridge.egress.udp.short stream=%u len=%zu\n", stream_id, payload_len);
            fflush(stdout);
            session->pending_close = 1;
            return 1;
        }
        addr_len = read_be16(payload + 9U);
        if (addr_len == 0U || payload_len < 13U + addr_len) {
            printf("bridge.egress.udp.addr_truncated stream=%u len=%zu want=%u\n", stream_id, payload_len, addr_len);
            fflush(stdout);
            session->pending_close = 1;
            return 1;
        }
        data_len = read_be16(payload + 11U + addr_len);
        if (payload_len < 13U + addr_len + data_len) {
            printf("bridge.egress.udp.data_truncated stream=%u len=%zu want=%u\n", stream_id, payload_len, data_len);
            fflush(stdout);
            session->pending_close = 1;
            return 1;
        }
        {
            /* 按 datagram 自带地址 sendto，而非发往固定首目标。 */
            char addr_text[256];
            char dst_host[256];
            uint16_t dst_port = 0U;
            struct sockaddr_storage dst;
            socklen_t dst_len = 0;
            int sent;
            if ((size_t) addr_len >= sizeof(addr_text)) {
                printf("bridge.egress.udp.addr_oversize stream=%u addr_len=%u\n", stream_id, addr_len);
                fflush(stdout);
                return 1; /* 丢弃该 datagram，不关流 */
            }
            memcpy(addr_text, payload + 11U, addr_len);
            addr_text[addr_len] = '\0';
            if (!bridge_split_hostport(addr_text, dst_host, sizeof(dst_host), &dst_port) ||
                !bridge_resolve_endpoint_cached(dst_host, dst_port, &dst, &dst_len)) {
                printf("bridge.egress.udp.addr_parse_fail stream=%u addr=%s\n", stream_id, addr_text);
                fflush(stdout);
                return 1; /* 解析失败：丢弃该 datagram，UDP 容错不关流 */
            }
            sent = sendto(session->fd, (const char *) (payload + 13U + addr_len), (int) data_len, 0,
                          (struct sockaddr *) &dst, (socklen_arg_t) dst_len);
            if (sent != (int) data_len) {
                /* unconnected UDP 单包发送失败（缓冲满/族不符等）只丢弃，不杀会话。 */
                printf("bridge.egress.udp.sendto.fail stream=%u target=%s len=%u sent=%d\n",
                       session->stream_id, addr_text, data_len, sent);
                fflush(stdout);
                return 1;
            }
            egress_session_note_first_send(session, data_len);
            egress_session_touch_activity(session);
            egress_session_note_send_chunk(session, data_len);
            if (XGW_BRIDGE_VERBOSE()) printf("bridge.egress.send stream=%u proto=udp target=%s len=%u\n",
                   session->stream_id,
                   addr_text,
                   data_len);
            fflush(stdout);
        }
        bridge_egress_drain_socket(session);
        return 1;
    }
    if (kind == XGW_BRIDGE_KIND_TCP_CLOSE) {
        egress_session_set_close_reason(session, "upstream_close");
        close_egress_session(session);
        return 1;
    }
    if (kind == XGW_BRIDGE_KIND_TCP_HALF_CLOSE) {
        if (!session->is_udp && session->fd >= 0) {
            if (xgw_shutdown_write(session->fd) == SOCKET_ERROR) {
                egress_session_set_close_reason(session, "target_half_close_fail");
                session->pending_close = 1;
                xgw_close_socket(session->fd);
                session->fd = -1;
                printf("bridge.egress.half_close.fail stream=%u target=%s:%u\n",
                       session->stream_id,
                       session->target_host,
                       session->target_port);
                fflush(stdout);
                return 1;
            }
            egress_session_touch_activity(session);
            printf("bridge.egress.half_close stream=%u target=%s:%u\n",
                   session->stream_id,
                   session->target_host,
                   session->target_port);
            fflush(stdout);
            bridge_egress_drain_socket(session);
        }
        return 1;
    }
    return 0;
}

void xgw_bridge_egress_poll_all(xgw_bridge_egress_t *bridge) {
    size_t i;
    size_t active = 0U;
    if (bridge == NULL) {
        return;
    }
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        xgw_bridge_egress_session_t *session = &bridge->sessions[i];
        if (!session->active || session->fd < 0) {
            continue;
        }
        active++;
        if (XGW_BRIDGE_VERBOSE()) {
            printf("bridge.egress.poll stream=%u proto=%s target=%s:%u local_port=%u pending=%zu\n",
                   session->stream_id,
                   session->is_udp ? "udp" : "tcp",
                   session->target_host,
                   session->target_port,
                   socket_local_port(session->fd),
                   session->pending_count);
            fflush(stdout);
        }
        bridge_egress_drain_socket(session);
    }
    if (active > 0U && XGW_BRIDGE_VERBOSE()) {
        printf("bridge.egress.poll.summary active=%zu\n", active);
        fflush(stdout);
    }
}

int xgw_bridge_egress_get_upstream(const xgw_bridge_egress_t *bridge,
                                   char *host,
                                   size_t host_len,
                                   uint16_t *port) {
    if (bridge == NULL || host == NULL || port == NULL) {
        return 0;
    }
    if (bridge->upstream_host[0] == '\0' || bridge->upstream_port == 0U) {
        return 0;
    }
    snprintf(host, host_len, "%s", bridge->upstream_host);
    *port = bridge->upstream_port;
    return 1;
}

size_t xgw_bridge_egress_active_count(const xgw_bridge_egress_t *bridge) {
    size_t i;
    size_t active = 0U;
    if (bridge == NULL) {
        return 0U;
    }
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        if (bridge->sessions[i].active) {
            active++;
        }
    }
    return active;
}

int xgw_bridge_egress_dequeue(xgw_bridge_egress_t *bridge,
                              uint8_t *buf,
                              size_t buf_cap,
                              size_t *out_len,
                              char *error,
                              size_t error_len) {
    return xgw_bridge_egress_dequeue_ex(bridge, buf, buf_cap, out_len, NULL, 0U, NULL, error, error_len);
}

int xgw_bridge_egress_dequeue_ex(xgw_bridge_egress_t *bridge,
                                 uint8_t *buf,
                                 size_t buf_cap,
                                 size_t *out_len,
                                 char *upstream_host,
                                 size_t upstream_host_len,
                                 uint16_t *upstream_port,
                                 char *error,
                                 size_t error_len) {
    size_t i;
    xgw_bridge_egress_session_t *best_pending = NULL;
    xgw_bridge_egress_session_t *best_close = NULL;
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (upstream_host != NULL && upstream_host_len > 0U) {
        upstream_host[0] = '\0';
    }
    if (upstream_port != NULL) {
        *upstream_port = 0U;
    }
    if (bridge == NULL) {
        return 0;
    }
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        xgw_bridge_egress_session_t *session = &bridge->sessions[i];
        if (!session->active) {
            continue;
        }
        if (session->fd >= 0) {
            bridge_egress_drain_socket(session);
        }
        if (session->pending_count > 0U) {
            if (best_pending == NULL || session->priority > best_pending->priority) {
                best_pending = session;
            }
            continue;
        }
        if (session->pending_close) {
            if (best_close == NULL || session->priority > best_close->priority) {
                best_close = session;
            }
        }
        if (session->fd < 0) {
            continue;
        }
    }
    if (best_pending != NULL) {
        xgw_bridge_egress_session_t *session = best_pending;
        uint8_t read_buf[XGW_BRIDGE_MAX_PAYLOAD];
        char read_addr[256];
        size_t data_len = 0U;
        size_t n;
        read_addr[0] = '\0';
        if (!egress_queue_pop_ex(session, read_addr, sizeof(read_addr), read_buf, sizeof(read_buf), &data_len)) {
            return 0;
        }
        if (session->is_udp) {
            char addr[320];
            if (read_addr[0] != '\0') {
                /* read_addr 是入站 recvfrom 记录的真实来源 "host:port"，直接使用。 */
                snprintf(addr, sizeof(addr), "%s", read_addr);
            } else {
                /* 仅在来源缺失时回退到首目标。 */
                snprintf(addr, sizeof(addr), "%s:%u", session->target_host, session->target_port);
            }
            n = encode_udp_data_payload(session->stream_id, addr, read_buf, data_len, buf, buf_cap);
        } else {
            n = encode_data_payload(session->stream_id, read_buf, data_len, buf, buf_cap);
        }
        if (n == 0U) {
            set_error(error, error_len, "egress data payload too large");
            return -1;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        if (upstream_host != NULL && upstream_host_len > 0U) {
            snprintf(upstream_host,
                     upstream_host_len,
                     "%s",
                     session->upstream_host[0] != '\0' ? session->upstream_host : bridge->last_upstream_host);
        }
        if (upstream_port != NULL) {
            *upstream_port = session->upstream_port != 0U ? session->upstream_port : bridge->last_upstream_port;
        }
        return 1;
    }
    if (best_close != NULL) {
        xgw_bridge_egress_session_t *session = best_close;
        size_t n = encode_close_payload(session->stream_id, buf, buf_cap);
        char close_upstream_host[64];
        uint16_t close_upstream_port;
        snprintf(close_upstream_host,
                 sizeof(close_upstream_host),
                 "%s",
                 session->upstream_host[0] != '\0' ? session->upstream_host : bridge->last_upstream_host);
        close_upstream_port = session->upstream_port != 0U ? session->upstream_port : bridge->last_upstream_port;
        close_egress_session(session);
        if (n == 0U) {
            set_error(error, error_len, "egress close payload too large");
            return -1;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        if (upstream_host != NULL && upstream_host_len > 0U) {
            snprintf(upstream_host, upstream_host_len, "%s", close_upstream_host);
        }
        if (upstream_port != NULL) {
            *upstream_port = close_upstream_port;
        }
        return 1;
    }
    return 0;
}

/* 从单条 session pop 一个 chunk、序列化成回程帧并回调 emit。
 * 返回：>0=已发出的帧字节数；0=该 session 已无 pending；-1=编码/回调错误。 */
static int egress_emit_one_chunk(xgw_bridge_egress_t *bridge,
                                 xgw_bridge_egress_session_t *session,
                                 uint8_t *frame,
                                 size_t frame_cap,
                                 xgw_bridge_egress_emit_fn emit,
                                 void *ctx,
                                 char *error,
                                 size_t error_len) {
    uint8_t read_buf[XGW_BRIDGE_MAX_PAYLOAD];
    char read_addr[256];
    size_t data_len = 0U;
    size_t n;
    char up_host[64];
    uint16_t up_port;
    int rc;
    read_addr[0] = '\0';
    if (!egress_queue_pop_ex(session, read_addr, sizeof(read_addr), read_buf, sizeof(read_buf), &data_len)) {
        return 0;
    }
    if (session->is_udp) {
        char addr[320];
        if (read_addr[0] != '\0') {
            /* read_addr 是入站 recvfrom 记录的真实来源 "host:port"，直接使用。 */
            snprintf(addr, sizeof(addr), "%s", read_addr);
        } else {
            /* 仅在来源缺失时回退到首目标。 */
            snprintf(addr, sizeof(addr), "%s:%u", session->target_host, session->target_port);
        }
        n = encode_udp_data_payload(session->stream_id, addr, read_buf, data_len, frame, frame_cap);
    } else {
        n = encode_data_payload(session->stream_id, read_buf, data_len, frame, frame_cap);
    }
    if (n == 0U) {
        set_error(error, error_len, "egress data payload too large");
        return -1;
    }
    snprintf(up_host, sizeof(up_host), "%s",
             session->upstream_host[0] != '\0' ? session->upstream_host : bridge->last_upstream_host);
    up_port = session->upstream_port != 0U ? session->upstream_port : bridge->last_upstream_port;
    rc = emit(ctx, frame, n, up_host, up_port);
    if (rc < 0) {
        return -1;
    }
    return (int) n;
}

int xgw_bridge_egress_dequeue_burst_ex(xgw_bridge_egress_t *bridge,
                                       size_t max_chunks,
                                       size_t max_bytes,
                                       uint64_t max_us,
                                       xgw_bridge_egress_emit_fn emit,
                                       void *ctx,
                                       char *error,
                                       size_t error_len) {
    /* 收集本轮带 pending 的 session 指针（单次全表扫描，含 drain）。 */
    xgw_bridge_egress_session_t *pend[XGW_BRIDGE_MAX_EGRESS_SESSIONS];
    size_t pend_n = 0U;
    size_t i;
    size_t emitted_chunks = 0U;
    size_t emitted_bytes = 0U;
    uint64_t start_us;
    uint8_t frame[XGW_BRIDGE_WIRE_PAYLOAD_MAX];

    if (bridge == NULL || emit == NULL) {
        return 0;
    }
    start_us = xgw_bridge_now_us();

    /* 第一遍：drain 所有 active session 的上游 socket，收集有 pending 的。 */
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        xgw_bridge_egress_session_t *session = &bridge->sessions[i];
        if (!session->active) {
            continue;
        }
        if (session->fd >= 0) {
            bridge_egress_drain_socket(session);
        }
        if (session->pending_count > 0U) {
            pend[pend_n++] = session;
        }
    }

    /* 第二遍：按优先级从高到低，组内 round-robin（DRR），在预算内连续吐多帧。
     * 选择排序代价 O(pend_n^2)，但 pend_n 受活跃流数约束、远小于全表 strcmp。 */
    {
        size_t ordered_done = 0U;
        while (ordered_done < pend_n) {
            /* 找出剩余里的最高优先级。 */
            uint32_t best_prio = 0U;
            int have = 0;
            size_t k;
            for (k = ordered_done; k < pend_n; ++k) {
                if (!have || pend[k]->priority > best_prio) {
                    best_prio = pend[k]->priority;
                    have = 1;
                }
            }
            /* 把该优先级的 session 收拢到 [ordered_done, group_end)。 */
            size_t group_start = ordered_done;
            size_t group_end = ordered_done;
            for (k = ordered_done; k < pend_n; ++k) {
                if (pend[k]->priority == best_prio) {
                    xgw_bridge_egress_session_t *tmp = pend[group_end];
                    pend[group_end] = pend[k];
                    pend[k] = tmp;
                    group_end++;
                }
            }
            /* 组内 round-robin：每轮每条流各吐 1 chunk，直到组内全空或预算耗尽。 */
            {
                size_t group_len = group_end - group_start;
                size_t cursor = group_len > 0U ? (bridge->rr_cursor % group_len) : 0U;
                size_t empty_streak = 0U;
                while (group_len > 0U && empty_streak < group_len) {
                    xgw_bridge_egress_session_t *session = pend[group_start + cursor];
                    int rc;
                    /* 预算检查（任一维度命中即停止整个 burst）。 */
                    if (max_chunks != 0U && emitted_chunks >= max_chunks) {
                        goto burst_done;
                    }
                    if (max_bytes != 0U && emitted_bytes >= max_bytes) {
                        goto burst_done;
                    }
                    if (max_us != 0U && (xgw_bridge_now_us() - start_us) >= max_us) {
                        goto burst_done;
                    }
                    rc = egress_emit_one_chunk(bridge, session, frame, sizeof(frame), emit, ctx, error, error_len);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc == 0) {
                        empty_streak++;
                    } else {
                        empty_streak = 0U;
                        emitted_chunks++;
                        emitted_bytes += (size_t) rc;
                    }
                    cursor = (cursor + 1U) % group_len;
                    bridge->rr_cursor++;
                }
            }
            ordered_done = group_end;
        }
    }
burst_done:

    /* 最后处理 close 帧（pending 数据已尽量发完，close 紧随其后保证顺序）。 */
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        xgw_bridge_egress_session_t *session = &bridge->sessions[i];
        if (!session->active || !session->pending_close || session->pending_count > 0U) {
            continue;
        }
        if (max_chunks != 0U && emitted_chunks >= max_chunks) {
            break;
        }
        {
            size_t n = encode_close_payload(session->stream_id, frame, sizeof(frame));
            char up_host[64];
            uint16_t up_port;
            int rc;
            snprintf(up_host, sizeof(up_host), "%s",
                     session->upstream_host[0] != '\0' ? session->upstream_host : bridge->last_upstream_host);
            up_port = session->upstream_port != 0U ? session->upstream_port : bridge->last_upstream_port;
            close_egress_session(session);
            if (n == 0U) {
                set_error(error, error_len, "egress close payload too large");
                return -1;
            }
            rc = emit(ctx, frame, n, up_host, up_port);
            if (rc < 0) {
                return -1;
            }
            emitted_chunks++;
        }
    }

    return (int) emitted_chunks;
}

void xgw_bridge_egress_free(xgw_bridge_egress_t *bridge) {
    size_t i;
    if (bridge == NULL) {
        return;
    }
    for (i = 0; i < XGW_BRIDGE_MAX_EGRESS_SESSIONS; ++i) {
        close_egress_session(&bridge->sessions[i]);
    }
    free(bridge);
}
