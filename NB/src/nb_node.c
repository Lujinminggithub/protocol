/*
 * Newbility(NB) 传输节点 —— 统一 entry / middle / exit 三角色
 *
 * 基于 picoquic(BBRv2) 的 TCP-over-QUIC 隧道节点。一份二进制,靠 -r 选角色:
 *   entry : 本地 TCP listen/accept, 复用一条到第一跳的 QUIC 长连接,
 *           每个 TCP 连接开一个 QUIC bidi stream, stream 首部发 source-route,
 *           TCP <-> QUIC stream 双向泵。
 *   middle: QUIC server 收上一跳 stream, 读首部第一段 H:host:port ->
 *           动态连下一跳 QUIC(地址来自首部,不预配), 转发剩余 route + 数据,
 *           上游 QUIC stream <-> 下游 QUIC stream 双向转发。中间节点无状态,
 *           路径完全由 entry 下发的 route 决定(适配"卖线路/动态选路")。
 *   exit  : QUIC server 收上一跳 stream, 读首部第一段 T:host:port ->
 *           connect 目标 TCP, QUIC stream <-> 目标 TCP 双向泵。
 *
 * source-route 首部(stream 第一行文本 + '\n'), 语义 = 接收节点出发的剩余路径:
 *   "H:nexthop:qport,...,T:targethost:targetport"
 *   H: 前缀 = 下一 QUIC 跳的地址(middle 消费第一个 H, 转发其余);
 *   T: 前缀 = 最终 TCP 目标(exit 消费)。
 *   两跳(entry 直连 exit): route 仅含 "T:target:port"(无 H)。
 *
 * 自定义事件循环: select(udp_fd + tcp_fds), 用 picoquic_incoming_packet /
 * picoquic_prepare_next_packet / picoquic_get_next_wake_delay 驱动 QUIC。
 *
 * 统一数据模型(left = 靠 client 一侧, right = 靠 target 一侧):
 *   entry : left=TCP(客户端)      right=下游 QUIC(第一跳)
 *   middle: left=上游 QUIC(上一跳) right=下游 QUIC(下一跳)
 *   exit  : left=上游 QUIC(上一跳) right=TCP(目标)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#ifdef __linux__
#include <execinfo.h>
#endif
#include <picoquic.h>
#include <picosocks.h>
#include <picoquic_utils.h>
#include <picoquic_bbr.h>
#include <picoquic_cubic.h>
#include <picoquic_fastcc.h>
#include <picoquic_internal.h>
#include <tls_api.h>
#include "nb_fec.h"
#include "nb_ring.h"
#include "nb_auth.h"
#include "nb_control.h"
#include "nb_routes.h"
#include "nb_policy.h"
#include "nb_udp.h"
#include "log/log4c.h"

#define NB_ALPN "nb/1"          /* Newbility(NB) 新传输协议 ALPN */
#define NB_SNI  "nb.internal"
#define BUFCAP (256*1024)
#define MAX_CONN 1024
#define NB_Q2T_MAX (16*1024*1024)   /* 过载保护: 单流 q2t 待写缓冲硬上限 16MB, 超限=对端TCP卡死 */
#define NB_QTX_MAX (16*1024*1024)   /* 过载保护: 单流 QUIC 侧应用发送缓冲硬上限 16MB */
#define NB_TCP_TX_HIGH (2*1024*1024)
#define NB_TCP_TX_LOW  (1024*1024)
#define NB_FEC_DG_HIGH (192*1024)
#define NB_FEC_DG_LOW  (96*1024)
#define NB_DGRAM_QUEUE_MAX (256*1024) /* FEC sidecar datagram 应用发送队列上限 */
#define NB_ROUTE_BOOTSTRAP_PRIO 0       /* latency 流仅在发送 route header 时临时提权 */
#define IDLE_TIMEOUT_US (120*1000000ULL)  /* 流空闲(无数据)超时兜底回收, 防僵尸流累积拖死复用连接 */
#define SOCKS_AUTH_CACHE_TTL_US (300*1000000ULL)
#define POOL_SIZE 1                        /* A/B 稳定基线：每跳仅一个 QUIC 拥塞控制实例 */
#define NB_MAX_POOLS 8
#define BULK_POOL_START 3                  /* 批量流优先使用后半池；实时流可选择全池 */
#define NB_FEC_CTRL_PREFIX "FC:"
#define NB_FEC_CTRL_START  "FC:START"
#define NB_FEC_CTRL_ACK    "FC:ACK"
#define NB_FEC_DGRAM_MAGIC 0x4E424644u /* 'NBFD' */
#define NB_FEC_DGRAM_VER   1
#define NB_FEC_V15_K 4
#define NB_FEC_V15_R 2

typedef enum {
    nb_fec_dgram_hello = 1,
    nb_fec_dgram_ack = 2,
    nb_fec_dgram_source = 3,
    nb_fec_dgram_repair = 4
} nb_fec_dgram_type_t;

typedef enum { ROLE_ENTRY = 0, ROLE_MIDDLE = 1, ROLE_EXIT = 2 } nb_role_t;

/* 每条被代理的流: left 侧 <-> right 侧, 双向缓冲/配对。 */
typedef struct proxy_stream {
    int in_use;
    uint32_t id;                 /* 本地流水号(日志追踪用) */
    /* 上游(left) QUIC: middle/exit 收上一跳的 server stream */
    picoquic_cnx_t* up_cnx;
    uint64_t up_stream_id;
    /* 下游(right) QUIC: entry/middle 到下一跳的 client stream */
    picoquic_cnx_t* down_cnx;
    uint64_t down_stream_id;
    int down_pool_idx;            /* 该下游主 stream 所在连接池槽位, 供附加控制流复用 */
    int down_pool_id;             /* 下一跳连接池注册表索引 */
    int down_opened;             /* 下游 stream 首部已发 */
    /* TCP: entry=客户端 conn; exit=目标 conn; -1=无 */
    int tcp_fd;
    int tcp_connecting;          /* exit: 非阻塞 connect 进行中 */
    /* 首部解析(middle/exit 收 up 首部) */
    int hdr_done;
    char hdr[600];
    size_t hdr_len;
    /* qtx: 面向 QUIC 的待发送缓冲。V1.5 起主数据路径改走 prepare_to_send，避免 add_to_stream
     * 额外排队复制。down_tx=left->right(发下游), up_tx=right->left(写回上游)。 */
    nb_ring_t down_tx;
    int down_tx_fin;
    uint8_t down_prefix[420];
    size_t down_prefix_len;
    size_t down_prefix_off;
    nb_ring_t up_tx;
    int up_tx_fin;
    /* q2t: 面向 TCP 的待写缓冲(entry 写回客户端 / exit 写目标)。动态增长, 永不丢数据;
     * 总量上限由 QUIC connection flow control(max_data) 自然背压 */
    nb_ring_t q2t;
    int q2t_fin;                 /* 待对 TCP 做半关 */
    int need_teardown;           /* 过载保护: q2t 超限, 主循环兜底回收(不在回调栈内拆) */
    int tcp_eof;                 /* 本地 TCP 读到 EOF */
    int tcp_read_paused;
    /* fin 追踪(去程 left->right, 回程 right->left) */
    int up_fin_seen;             /* 收到 left QUIC fin(仅 middle/exit) */
    int down_fin_seen;           /* 收到 right QUIC fin(仅 entry/middle) */
    char route[300];             /* 本 stream 的 route 首部(日志) */
    uint64_t last_active;        /* 最近一次有数据活动的时刻(us), 空闲超时兜底回收用 */
    int dns_pending;             /* exit: 该流的 target 域名正在异步 DNS 解析中 */
    int target_port;             /* exit: DNS 解析完成后 connect 用的端口 */
    int prio;                    /* 流优先级(例如 media=4 / ctrl=8 / bulk=20), 首部 <prio>; 端到端传递 */
    nb_flow_class_t flow_class;  /* 白名单通过后, TikTok 内部分流分类(ctrl/media/bulk/unknown) */
    nb_flow_lane_t lane_hint;    /* 分流器给出的 lane 建议(latency/bulk) */
    nb_flow_fec_t fec_hint;      /* 分流器给出的 FEC 建议(auto/off/force) */
    char flow_rule[64];          /* 命中的 TikTok 规则名 */
    /* ===== V1.5 纠错型 FEC sidecar 骨架 =====
     * 保持当前 TCP-over-QUIC 主路径不变，在坏跳 middle<->exit 内部额外建立：
     * 1) 一条可靠控制流(FEC START/ACK/NACK/RETX/FIN/RST)
     * 2) 一条连接级 datagram sidecar(先打通 HELLO/ACK 骨架，后续再承载 source/repair symbol) */
    int fec_sidecar_mode;        /* 当前逻辑流是否已切换为 V1.5 sidecar 数据面 */
    int fec_ctrl_only;           /* 该 ps 仅服务于 FEC 控制流(如 exit 收到的控制会话) */
    uint32_t fec_session_id;     /* V1.5 FEC 会话 ID */
    picoquic_cnx_t* fec_ctrl_cnx;/* middle 本地额外打开的 FEC 控制流所在线路 */
    uint64_t fec_ctrl_stream_id;
    int fec_ctrl_opened;
    int fec_ctrl_peer_ready;
    int fec_rx_to_down;          /* sidecar 接收恢复后应继续往下游转发(如 entry->middle 保护段) */
    char fec_route[300];         /* sidecar 会话携带的坏跳目标 route(一般为 T:host:port) */
    char fec_ctrl_rxbuf[4096];
    size_t fec_ctrl_rxlen;
    picoquic_cnx_t* fec_dg_cnx;  /* sidecar datagram 所属连接(目前与控制流/坏跳数据流同 cnx) */
    uint8_t* fec_dg_tx;
    size_t fec_dg_tx_len;
    size_t fec_dg_tx_cap;
    uint8_t* fec_stage_tx;
    size_t fec_stage_tx_len;
    size_t fec_stage_tx_cap;
    int fec_stage_tx_fin;
    uint64_t fec_dg_sent;
    uint64_t fec_dg_recv;
    uint64_t fec_dg_acked;
    uint64_t fec_dg_lost;
    nb_fec_session_t* fec_engine;
    /* SOCKS5 入口(仅 entry -S): 握手阶段 0=greeting 1=request 2=直通 */
    int socks_stage;
    uint8_t socks_buf[512];
    size_t socks_len;
    uint64_t created_at;
    uint64_t first_c2s_at;
    uint64_t first_s2c_at;
    uint64_t bytes_c2s;
    uint64_t bytes_s2c;
    uint64_t target_connect_at;
    uint64_t target_connect_done_at;
    uint64_t socks_greeting_at;
    uint64_t socks_auth_at;
    uint64_t socks_request_at;
    uint64_t downstream_open_at;
    uint64_t down_prefix_queued_at;
    uint64_t down_prefix_prepare_at;
    uint64_t first_down_queue_at;
    uint64_t first_down_prepare_at;
    uint64_t first_up_queue_at;
    uint64_t first_up_prepare_at;
    uint64_t first_q2t_queue_at;
    uint64_t first_q2t_flush_at;
    int target_connect_state;    /* 0=n/a, 1=pending, 2=ok, 3=failed */
    char peer_addr[96];
    char close_reason[48];
    int udp_mode;
    int udp_association;
    int udp_fd;
    uint32_t udp_session_id;
    uint32_t udp_parent_id;
    uint32_t udp_tx_sequence;
    char udp_target_host[256];
    int udp_target_port;
    struct sockaddr_storage udp_tcp_peer;
    socklen_t udp_tcp_peer_len;
    struct sockaddr_storage udp_client_peer;
    socklen_t udp_client_peer_len;
    int udp_client_peer_set;
    uint8_t* udp_down_tx;
    size_t udp_down_tx_len;
    size_t udp_down_tx_cap;
    uint8_t* udp_up_tx;
    size_t udp_up_tx_len;
    size_t udp_up_tx_cap;
    uint8_t* udp_pending_tx;
    size_t udp_pending_tx_len;
    size_t udp_pending_tx_cap;
    nb_udp_reassembly_t udp_reassembly;
    uint64_t udp_packets_c2s;
    uint64_t udp_packets_s2c;
    uint64_t udp_first_local_c2s_at;
    uint64_t udp_first_c2s_rx_at;
    uint64_t udp_first_c2s_prepare_at;
    uint64_t udp_target_ready_at;
    uint64_t udp_first_target_send_at;
    uint64_t udp_first_target_rx_at;
    uint64_t udp_first_s2c_rx_at;
    uint64_t udp_first_s2c_prepare_at;
    uint64_t udp_first_local_s2c_at;
} proxy_stream_t;

/* 到下一跳的 QUIC 连接池: 每条连接独立 congestion window / pacing / flow-control,
 * 新 stream round-robin 分散到各连接, 使总吞吐叠加而非稀释, 且单连接故障/僵死不影响全局。 */
typedef struct cnx_pool {
    picoquic_cnx_t* cnx[POOL_SIZE];  /* 池中连接(NULL=空槽, 待建/已关) */
    uint64_t next_sid[POOL_SIZE];    /* 每条连接独立的 client bidi stream id 分配 0,4,8... */
    struct sockaddr_storage addr;    /* 下一跳地址 */
    int configured;                  /* addr 已配、池已初始化 */
    int rr;                          /* 批量道 round-robin 游标 */
    int rr_lat;                      /* 直播专用道 round-robin 游标 */
    double recent_loss[POOL_SIZE];   /* 最近一个采样窗(增量)的有效丢包率(%) */
    uint64_t recent_rtt[POOL_SIZE];  /* 最近一次 linkq 采样的 RTT(us) */
    uint64_t recent_rtt_max[POOL_SIZE]; /* 最近一次 linkq 采样的最大 RTT(us) */
    uint64_t recent_sent[POOL_SIZE]; /* 最近一个采样窗(增量)的发包数 */
    uint64_t last_sent_total[POOL_SIZE]; /* 上次采样时的累计 sent */
    uint64_t last_lost_total[POOL_SIZE]; /* 上次采样时的累计 lost */
    uint64_t last_timer_total[POOL_SIZE]; /* 上次采样时的累计 timer loss */
    uint64_t last_spurious_total[POOL_SIZE]; /* 上次采样时的累计 spurious_losses */
    uint64_t last_retrans_total[POOL_SIZE]; /* 上次采样时的累计实际重传 */
    uint64_t last_preempt_total[POOL_SIZE]; /* 上次采样时的累计预防性重传 */
    uint64_t recent_rtt_var[POOL_SIZE]; /* 最近一次采样窗对应的 RTT 抖动(us) */
    uint64_t recent_ts[POOL_SIZE];   /* 最近一次 linkq 采样时间(us), 0=暂无样本 */
    int fec_latched;                 /* FEC 观察门控的回滞状态 */
} cnx_pool_t;

typedef struct nb_global {
    nb_role_t role;
    picoquic_quic_t* quic;
    int udp_fd;                          /* QUIC UDP socket */
    int epoll_fd;
    int control_fd;
    struct sockaddr_storage local_addr;  /* incoming_packet 的 addr_to */
    struct sockaddr_storage outbound_addr; /* exit 目标 TCP 的指定源地址 */
    int outbound_configured;
    /* entry: 本地 TCP listen */
    int tcp_listen_fd;
    char route_str[300];                 /* entry: 发给第一跳的 route(固定 target 模式) */
    int socks_enabled;                   /* entry: SOCKS5 入口(动态 target) */
    char mid_route[256];                 /* entry SOCKS 模式: 中间跳前缀 "H:kz:4443"(可空=两跳) */
    nb_routes_t exit_routes;
    int exit_routes_enabled;
    /* entry/middle: 到下一跳的 QUIC 连接池(每条独立 cwnd/pacing/flow-control, round-robin 分流) */
    cnx_pool_t pools[NB_MAX_POOLS];
    int pool_count;
    struct sockaddr_storage next_addr;    /* entry: 第一跳地址(启动配, pool_init 用) */
    int next_configured;                  /* entry: next_addr 已配 */
    uint32_t next_ps_id;                  /* proxy_stream 流水号分配 */
    proxy_stream_t streams[MAX_CONN];
} nb_global_t;

static nb_global_t G;
static nb_auth_users_t g_socks_users;
static int g_socks_auth_enabled=0;
static struct in_addr g_socks_udp_advertise_addr;
static int g_socks_udp_advertise_configured=0;
static uint16_t g_socks_udp_port_min=0;
static uint16_t g_socks_udp_port_max=0;
static uint16_t g_socks_udp_port_next=0;
static int g_ps_inuse=0, g_ps_peak=0;   /* 过载保护: 在用流数 + 峰值 */
static int g_udp_gso_enabled=1;
static uint64_t g_reorder_gap=3;
static uint64_t g_reorder_delay_us=0;
static int g_fec_v15_observe=1;         /* 默认只观测门控，不接管业务数据；NB_FEC_V15=off/0 可关闭观测 */
static int g_fec_v15_active=0;          /* 仅 NB_FEC_V15_ACTIVE=on/1 显式启用 sidecar 数据面 */
static int g_fec_v15_force=0;           /* V1.5 测试强开: 忽略 line_bad, 便于代码级测试 */
static uint32_t g_fec_v15_drop_src_mod=0;    /* 测试钩子: 每 N 个 source datagram 丢 1 个(收端侧) */
static uint32_t g_fec_v15_drop_repair_mod=0; /* 测试钩子: 每 N 个 repair datagram 丢 1 个(收端侧) */
static uint64_t g_fec_v15_drop_src=0;
static uint64_t g_fec_v15_drop_repair=0;
static nb_fec_config_t g_fec_cfg;
static nb_fec_metrics_t g_fec_metrics_done;
#define NB_FEC_SESSION_INDEX_CAP 2048u
typedef struct { uint32_t sid; proxy_stream_t* stream; } nb_fec_session_index_t;
static nb_fec_session_index_t g_fec_session_index[NB_FEC_SESSION_INDEX_CAP];
#define NB_EPOLL_TAG_UDP 1ULL
#define NB_EPOLL_TAG_DNS 2ULL
#define NB_EPOLL_TAG_LISTEN 3ULL
#define NB_EPOLL_TAG_CONTROL 4ULL
#define NB_EPOLL_TAG_STREAM (1ULL<<63)
#define NB_EPOLL_TAG_UDP_SESSION (1ULL<<62)

static void fec_metrics_add(nb_fec_metrics_t* dst, const nb_fec_metrics_t* src){
    if(dst==NULL||src==NULL) return;
    uint64_t* d=(uint64_t*)dst; const uint64_t* s=(const uint64_t*)src;
    for(size_t i=0;i<sizeof(*dst)/sizeof(uint64_t);i++) d[i]+=s[i];
}

static proxy_stream_t* fec_session_index_find(uint32_t sid){
    if(sid==0) return NULL;
    uint32_t pos=sid%NB_FEC_SESSION_INDEX_CAP;
    for(uint32_t i=0;i<NB_FEC_SESSION_INDEX_CAP;i++){
        nb_fec_session_index_t* e=&g_fec_session_index[(pos+i)%NB_FEC_SESSION_INDEX_CAP];
        if(e->sid==0) return NULL;
        if(e->sid==sid) return e->stream;
    }
    return NULL;
}
static int fec_session_index_add(uint32_t sid,proxy_stream_t* p){
    uint32_t pos=sid%NB_FEC_SESSION_INDEX_CAP;int tomb=-1;
    for(uint32_t i=0;i<NB_FEC_SESSION_INDEX_CAP;i++){
        uint32_t at=(pos+i)%NB_FEC_SESSION_INDEX_CAP;nb_fec_session_index_t* e=&g_fec_session_index[at];
        if(e->sid==sid)return e->stream==p?0:-1;
        if(e->sid==UINT32_MAX&&tomb<0)tomb=(int)at;
        if(e->sid==0){if(tomb>=0)e=&g_fec_session_index[tomb];e->sid=sid;e->stream=p;return 0;}
    }return -1;
}
static void fec_session_index_remove(uint32_t sid,proxy_stream_t* p){
    uint32_t pos=sid%NB_FEC_SESSION_INDEX_CAP;
    for(uint32_t i=0;i<NB_FEC_SESSION_INDEX_CAP;i++){
        nb_fec_session_index_t* e=&g_fec_session_index[(pos+i)%NB_FEC_SESSION_INDEX_CAP];
        if(e->sid==0) return;
        if(e->sid==sid&&e->stream==p){e->sid=UINT32_MAX;e->stream=NULL;return;}
    }
}

static uint32_t fec_session_id_new(void){
    for(int attempt=0;attempt<16;attempt++){
        uint32_t sid=0;picoquic_crypto_random(G.quic,&sid,sizeof(sid));
        if(sid!=0&&fec_session_index_find(sid)==NULL)return sid;
    }
    return 0;
}

/* ===== 异步 DNS 子系统 =====
 * exit 的 target 域名解析(getaddrinfo)是阻塞调用; 若在单线程事件循环内同步执行, 高并发下
 * 每个新域名解析会冻结整个事件循环 -> 所有视频/直播流暂停 -> 直播规律性卡顿。
 * 改为独立 worker 线程池: 主线程提交请求 -> worker getaddrinfo -> 结果入完成队列 + self-pipe
 * 唤醒 select -> 主线程发起非阻塞 connect。用 proxy_stream 流水号 ps_id 关联(不跨线程传指针),
 * 避免解析期间流被释放的 use-after-free。 */
#define DNS_QCAP 4096
#define DNS_WORKERS 4

typedef struct dns_req { uint32_t ps_id; int port; char host[256]; } dns_req_t;
typedef struct dns_res { uint32_t ps_id; int port; int ok; struct sockaddr_storage addr; socklen_t addrlen; } dns_res_t;

static int queue_down(proxy_stream_t* p, const uint8_t* d, size_t n, int fin);
static int queue_up(proxy_stream_t* p, const uint8_t* d, size_t n, int fin);
static void fwd_down(proxy_stream_t* p, uint8_t* d, size_t n, int fin);
static void fwd_up(proxy_stream_t* p, uint8_t* d, size_t n, int fin);
static int pool_init(cnx_pool_t* pool, struct sockaddr_storage* addr);
static int pool_get_or_create(struct sockaddr_storage* addr);
static int downstream_open_stream(proxy_stream_t* p, const char* route, const nb_flow_policy_t* pol);
static int fec_parse_target_route(const char* route, char* host, size_t host_cap, int* port);
static int epoll_tcp_update(proxy_stream_t* p, int add);

static struct {
    dns_req_t req[DNS_QCAP]; int rq_head, rq_tail;   /* 请求环形队列 */
    dns_res_t res[DNS_QCAP]; int rs_head, rs_tail;   /* 完成环形队列 */
    pthread_mutex_t mu; pthread_cond_t cv;
    int pipe_rd, pipe_wr;                            /* self-pipe 唤醒主 select */
    int started;
} DNS;

static int dns_q_next(int i){ return (i+1)%DNS_QCAP; }

/* worker: 阻塞取请求 -> getaddrinfo -> 结果入完成队列 + 写 self-pipe 唤醒主循环 */
static void* dns_worker(void* arg){
    (void)arg;
    for(;;){
        dns_req_t rq;
        pthread_mutex_lock(&DNS.mu);
        while(DNS.rq_head==DNS.rq_tail) pthread_cond_wait(&DNS.cv,&DNS.mu);
        rq=DNS.req[DNS.rq_head]; DNS.rq_head=dns_q_next(DNS.rq_head);
        pthread_mutex_unlock(&DNS.mu);

        struct addrinfo hints, *res=NULL; char ports[16];
        memset(&hints,0,sizeof(hints));
        hints.ai_family=G.outbound_configured?G.outbound_addr.ss_family:AF_UNSPEC;
        hints.ai_socktype=SOCK_STREAM;
        snprintf(ports,sizeof(ports),"%d",rq.port);
        dns_res_t out; memset(&out,0,sizeof(out)); out.ps_id=rq.ps_id; out.port=rq.port;
        if(getaddrinfo(rq.host,ports,&hints,&res)==0 && res){
            memcpy(&out.addr,res->ai_addr,res->ai_addrlen); out.addrlen=res->ai_addrlen; out.ok=1;
            freeaddrinfo(res);
        }
        pthread_mutex_lock(&DNS.mu);
        int nt=dns_q_next(DNS.rs_tail);
        if(nt!=DNS.rs_head){ DNS.res[DNS.rs_tail]=out; DNS.rs_tail=nt; } /* 满则丢弃, 该流靠空闲超时回收 */
        pthread_mutex_unlock(&DNS.mu);
        ssize_t w=write(DNS.pipe_wr,"x",1); (void)w;
    }
    return NULL;
}

/* 初始化 DNS 线程池 + self-pipe */
static int dns_init(void){
    int pf[2];
    if(pipe(pf)!=0){ log4c_error("dns pipe fail: %s",strerror(errno)); return -1; }
    DNS.pipe_rd=pf[0]; DNS.pipe_wr=pf[1];
    fcntl(DNS.pipe_rd,F_SETFL,fcntl(DNS.pipe_rd,F_GETFL,0)|O_NONBLOCK);
    fcntl(DNS.pipe_wr,F_SETFL,fcntl(DNS.pipe_wr,F_GETFL,0)|O_NONBLOCK);
    pthread_mutex_init(&DNS.mu,NULL); pthread_cond_init(&DNS.cv,NULL);
    for(int i=0;i<DNS_WORKERS;i++){ pthread_t t; if(pthread_create(&t,NULL,dns_worker,NULL)==0) pthread_detach(t); }
    DNS.started=1;
    log4c_info("async DNS: %d workers ready",DNS_WORKERS);
    return 0;
}

/* 主线程提交解析请求(非阻塞)。0=入队, -1=队满 */
static int dns_submit(uint32_t ps_id, const char* host, int port){
    pthread_mutex_lock(&DNS.mu);
    int nt=dns_q_next(DNS.rq_tail);
    if(nt==DNS.rq_head){ pthread_mutex_unlock(&DNS.mu); return -1; }
    DNS.req[DNS.rq_tail].ps_id=ps_id; DNS.req[DNS.rq_tail].port=port;
    snprintf(DNS.req[DNS.rq_tail].host,sizeof(DNS.req[DNS.rq_tail].host),"%s",host);
    DNS.rq_tail=nt;
    pthread_cond_signal(&DNS.cv);
    pthread_mutex_unlock(&DNS.mu);
    return 0;
}

/* 主线程取一个完成结果(非阻塞)。1=取到, 0=空 */
static int dns_pop_result(dns_res_t* out){
    pthread_mutex_lock(&DNS.mu);
    if(DNS.rs_head==DNS.rs_tail){ pthread_mutex_unlock(&DNS.mu); return 0; }
    *out=DNS.res[DNS.rs_head]; DNS.rs_head=dns_q_next(DNS.rs_head);
    pthread_mutex_unlock(&DNS.mu);
    return 1;
}

/* ---- 工具 ---- */
static int set_nonblock(int fd){ int fl=fcntl(fd,F_GETFL,0); return fl<0?-1:fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
static const char* role_name(nb_role_t r){ return r==ROLE_ENTRY?"entry":(r==ROLE_MIDDLE?"middle":"exit"); }

static const char* cc_state_name(const char* alg,uint64_t state){
    if(alg&&(!strcmp(alg,"cubic")||!strcmp(alg,"dcubic"))){
        if(state==0)return "slow_start";
        if(state==1)return "recovery";
        if(state==2)return "avoidance";
    }else if(alg&&!strcmp(alg,"fast")){
        if(state==0)return "initial";
        if(state==1)return "eval";
        if(state==2)return "freeze";
    }
    return "numeric";
}
static int prio_is_latency(int prio){ return nb_prio_is_latency(prio); }
static int prio_is_fec_candidate(int prio){ return nb_prio_is_fec_candidate(prio); }

static int control_render(const char* command,char* out,size_t cap,void* ctx){
    (void)ctx;
    const char* worker=getenv("NB_WORKER_ID");if(!worker)worker="0";
    if(!strcmp(command,"health")||!strcmp(command,"GET /health")){
        return snprintf(out,cap,"{\"status\":\"ok\",\"role\":\"%s\",\"worker\":\"%s\"}\n",role_name(G.role),worker);
    }
    if(!strcmp(command,"metrics")||!strcmp(command,"GET /metrics")){
        return snprintf(out,cap,
            "{\"role\":\"%s\",\"worker\":\"%s\",\"sessions\":%d,\"sessions_peak\":%d,\"pools\":%d,"
            "\"exit_routes\":%zu,\"fec_observe\":%d,\"fec_active\":%d,\"fec_tx_blocks\":%llu,\"fec_rx_blocks\":%llu,"
            "\"fec_recovered\":%llu,\"fec_nack\":%llu,\"fec_retx\":%llu}\n",
            role_name(G.role),worker,g_ps_inuse,g_ps_peak,G.pool_count,G.exit_routes.count,g_fec_v15_observe,g_fec_v15_active,
            (unsigned long long)g_fec_metrics_done.blocks_encoded,(unsigned long long)g_fec_metrics_done.blocks_delivered,
            (unsigned long long)g_fec_metrics_done.blocks_recovered,(unsigned long long)g_fec_metrics_done.nack_sent,
            (unsigned long long)g_fec_metrics_done.retx_sent);
    }
    return -1;
}


/* ===== 白名单(访问控制) =====
 * entry 收到 SOCKS CONNECT(host:port): 命中(域名后缀 / IP CIDR / 端口)才允许走隧道, 未命中直接拒绝。
 * 目的: 防止无关流量走三跳线路占带宽 + 降低安全风险。启动加载 + 文件 mtime 热重载。entry 单线程, 无锁。
 * 规则: host命中(域名或IP) AND port命中; 某类白名单为空 = 该维度不限制。 */
#define WL_MAX 1024
typedef struct { uint32_t net, mask; } wl_cidr_t;
typedef struct {
    char domain[WL_MAX][128]; int n_dom;   /* 域名后缀 */
    wl_cidr_t ip[WL_MAX];     int n_ip;    /* IPv4 CIDR */
    uint16_t port[WL_MAX];    int n_port;  /* 端口 */
    int enabled;                           /* 有配置文件才启用 */
    char path[256];
    time_t mtime;
} wl_state_t;
static wl_state_t WL;

static int wl_domain_hit(const char* host){
    size_t hl=strlen(host);
    for(int i=0;i<WL.n_dom;i++){
        size_t dl=strlen(WL.domain[i]);
        if(hl==dl && strcasecmp(host,WL.domain[i])==0) return 1;                       /* 完全相等 */
        if(hl>dl && host[hl-dl-1]=='.' && strcasecmp(host+hl-dl,WL.domain[i])==0) return 1; /* .后缀边界对齐 */
    }
    return 0;
}
static int wl_ip_hit(const char* host){
    struct in_addr a; if(inet_pton(AF_INET,host,&a)!=1) return 0;
    uint32_t ip=ntohl(a.s_addr);
    for(int i=0;i<WL.n_ip;i++) if((ip & WL.ip[i].mask)==WL.ip[i].net) return 1;
    return 0;
}
static int wl_port_hit(int port){
    if(WL.n_port==0) return 1;                       /* 未配端口 = 不限制端口 */
    for(int i=0;i<WL.n_port;i++) if(WL.port[i]==(uint16_t)port) return 1;
    return 0;
}
/* 命中判定: 未启用=全放行; host 是 IP 字面量走 CIDR, 否则走域名后缀 */
static int whitelist_allowed(const char* host, int port){
    if(!WL.enabled) return 1;
    if(!wl_port_hit(port)) return 0;
    struct in_addr a;
    /* 每个维度独立, 该维度未配=不限制。修复: 只配域名白名单时, IP 直连目标(如直播媒体流)不应被误拦。 */
    if(inet_pton(AF_INET,host,&a)==1) return WL.n_ip==0 ? 1 : wl_ip_hit(host);
    return WL.n_dom==0 ? 1 : wl_domain_hit(host);
}
static int wl_load(const char* path){
    FILE* f=fopen(path,"r");
    if(!f){ log4c_error("whitelist %s open fail: %s",path,strerror(errno)); return -1; }
    wl_state_t next;
    memset(&next,0,sizeof(next));
    snprintf(next.path,sizeof(next.path),"%s",path);
    char line[512];
    while(fgets(line,sizeof(line),f)){
        char* p=line; while(*p==' '||*p=='\t')p++;
        if(*p=='#'||*p=='\n'||*p=='\r'||*p==0) continue;
        char kw[16], val[256];
        if(sscanf(p,"%15s %255s",kw,val)!=2) continue;
        if(!strcmp(kw,"domain")){
            size_t n=strlen(val);
            if(n==0||n>=sizeof(next.domain[0])){fclose(f);log4c_error("whitelist invalid domain length");return -1;}
            if(next.n_dom<WL_MAX){memcpy(next.domain[next.n_dom],val,n+1);next.n_dom++;}
        }
        else if(!strcmp(kw,"port")){
            int port=atoi(val);
            if(port<=0||port>65535){fclose(f);log4c_error("whitelist invalid port: %s",val);return -1;}
            if(next.n_port<WL_MAX) next.port[next.n_port++]=(uint16_t)port;
        }
        else if(!strcmp(kw,"ip")){
            int len=32; char* slash=strchr(val,'/'); if(slash){ *slash=0; len=atoi(slash+1); }
            struct in_addr a;
            if(inet_pton(AF_INET,val,&a)==1 && len>=0 && len<=32 && next.n_ip<WL_MAX){
                uint32_t mask = len==0?0u:(0xFFFFFFFFu << (32-len));
                next.ip[next.n_ip].mask=mask; next.ip[next.n_ip].net=ntohl(a.s_addr)&mask; next.n_ip++;
            } else {fclose(f);log4c_error("whitelist invalid cidr: %s",val);return -1;}
        }
        else {fclose(f);log4c_error("whitelist unknown directive: %s",kw);return -1;}
    }
    fclose(f);
    if(next.n_dom==0&&next.n_ip==0){log4c_error("whitelist has no domain or cidr rules: %s",path);return -1;}
    struct stat st;if(stat(path,&st)==0)next.mtime=st.st_mtime;
    next.enabled=1;
    WL=next;
    log4c_info("whitelist loaded: %d domains, %d cidrs, %d ports (%s)",WL.n_dom,WL.n_ip,WL.n_port,path);
    return 0;
}
static int wl_init(const char* path){
    if(!path||!path[0]) return 0;
    return wl_load(path);
}
static void wl_reload_if_changed(void){
    if(!WL.path[0]) return;
    struct stat st;
    if(stat(WL.path,&st)==0 && st.st_mtime!=WL.mtime){
        if(wl_load(WL.path)!=0) log4c_error("whitelist reload rejected; keeping previous rules");
    }
}

static int parse_port_strict(const char* text,int* port){
    char* end=NULL;long value=strtol(text,&end,10);
    if(text==end||*end||value<=0||value>65535)return -1;
    *port=(int)value;return 0;
}

static int parse_target_from_route(const char* route, char* host, size_t host_cap, int* port){
    const char* t = strstr(route, "T:");
    if(t == NULL) return -1;
    char tmp[300];
    const char* start=t+2;const char* comma=strchr(start,',');size_t len=comma?(size_t)(comma-start):strlen(start);
    if(len==0||len>=sizeof(tmp))return -1;
    memcpy(tmp,start,len);tmp[len]=0;
    char* c = strrchr(tmp, ':');
    if(c == NULL) return -1;
    *c = 0;
    size_t host_len=strlen(tmp);if(host_len==0||host_len>=host_cap)return -1;memcpy(host,tmp,host_len+1);
    return parse_port_strict(c+1,port);
}

static void flow_policy_for_host(const char* host, int port, nb_flow_policy_t* out){
    nb_flow_policy_default(out);
    (void)nb_tiktok_flow_classify(host, port, out);
}

static void apply_flow_policy(proxy_stream_t* p, const nb_flow_policy_t* pol){
    p->flow_class = pol->flow_class;
    p->lane_hint = pol->lane_hint;
    p->fec_hint = pol->fec_hint;
    p->prio = pol->prio;
    snprintf(p->flow_rule, sizeof(p->flow_rule), "%s", pol->rule_name);
}

static int ps_is_rtc_webcast(const proxy_stream_t* p){
    return p != NULL && (p->flow_class == NB_FLOW_CLASS_MEDIA ||
        strstr(p->route, "rtc-") != NULL || strstr(p->route, "rtcpc-") != NULL ||
        strstr(p->route, "webcast") != NULL);
}

static void ps_set_close_reason(proxy_stream_t* p, const char* reason){
    if(p != NULL && p->close_reason[0] == 0 && reason != NULL && reason[0] != 0){
        snprintf(p->close_reason, sizeof(p->close_reason), "%s", reason);
    }
}

static const char* ps_connect_state_name(int state){
    switch(state){
    case 1: return "pending";
    case 2: return "ok";
    case 3: return "failed";
    default: return "n/a";
    }
}

static void ps_note_payload(proxy_stream_t* p, int c2s, size_t len){
    if(p == NULL || len == 0) return;
    uint64_t now = picoquic_current_time();
    uint64_t* total = c2s ? &p->bytes_c2s : &p->bytes_s2c;
    uint64_t* first = c2s ? &p->first_c2s_at : &p->first_s2c_at;
    *total += len;
    if(*first == 0){
        *first = now;
        if(ps_is_rtc_webcast(p)){
            log4c_info("%s media flow first-byte id=%u dir=%s age=%.1fms bytes=%zu route=%s",
                role_name(G.role), p->id, c2s ? "c2s" : "s2c",
                (now - p->created_at) / 1000.0, len, p->route);
        }
    }
}

static double trace_delta_ms(uint64_t end, uint64_t start){
    return end && start && end >= start ? (end - start) / 1000.0 : -1.0;
}

static void sockaddr_to_text(const struct sockaddr* sa, socklen_t sl, char* out, size_t cap){
    char host[NI_MAXHOST], service[NI_MAXSERV];
    if(out == NULL || cap == 0) return;
    if(sa != NULL && getnameinfo(sa, sl, host, sizeof(host), service, sizeof(service),
        NI_NUMERICHOST | NI_NUMERICSERV) == 0){
        if(sa->sa_family == AF_INET6) snprintf(out, cap, "[%.64s]:%.8s", host, service);
        else snprintf(out, cap, "%.64s:%.8s", host, service);
    }else{
        snprintf(out, cap, "unknown");
    }
}

static void socks_methods_to_text(const uint8_t* methods, uint8_t count, char* out, size_t cap){
    size_t used = 0;
    if(out == NULL || cap == 0) return;
    out[0] = 0;
    for(uint8_t i = 0; i < count && used + 4 < cap; i++){
        int n = snprintf(out + used, cap - used, "%s0x%02x", i ? "," : "", methods[i]);
        if(n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}

static const char* socks_cmd_name(uint8_t cmd){
    switch(cmd){
    case 0x01: return "CONNECT";
    case 0x02: return "BIND";
    case 0x03: return "UDP_ASSOCIATE";
    default: return "UNKNOWN";
    }
}

static const char* socks_atyp_name(uint8_t atyp){
    switch(atyp){
    case 0x01: return "IPv4";
    case 0x03: return "DOMAIN";
    case 0x04: return "IPv6";
    default: return "UNKNOWN";
    }
}

static void fmt_route_preview(const char* in, char* out, size_t cap){
    static const char hx[] = "0123456789ABCDEF";
    size_t oi = 0;
    if(cap == 0) return;
    out[0] = 0;
    for(size_t i=0; in[i] != 0 && oi + 1 < cap; i++){
        unsigned char ch = (unsigned char)in[i];
        if(isprint(ch)){
            out[oi++] = (char)ch;
        } else {
            if(oi + 4 >= cap) break;
            out[oi++] = '\\';
            out[oi++] = 'x';
            out[oi++] = hx[(ch >> 4) & 0xF];
            out[oi++] = hx[ch & 0xF];
        }
        if(i >= 95){
            if(oi + 4 < cap){
                out[oi++] = '.';
                out[oi++] = '.';
                out[oi++] = '.';
            }
            break;
        }
    }
    out[oi] = 0;
}

static const char* nb_sig_name(int sig){
    switch(sig){
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGPIPE: return "SIGPIPE";
    case SIGTERM: return "SIGTERM";
    case SIGINT: return "SIGINT";
    default: return "SIGNAL";
    }
}

static void nb_fatal_signal_handler(int sig, siginfo_t* si, void* uctx){
    (void)uctx;
    void* bt[32];
    int n = 0;
#ifdef __linux__
    n = backtrace(bt, (int)(sizeof(bt) / sizeof(bt[0])));
#endif
    dprintf(STDERR_FILENO, "\n[FATAL] nb_node signal=%d(%s) addr=%p code=%d errno=%d\n",
        sig, nb_sig_name(sig), si ? si->si_addr : NULL, si ? si->si_code : 0, si ? si->si_errno : 0);
#ifdef __linux__
    if(n > 0) backtrace_symbols_fd(bt, n, STDERR_FILENO);
#endif
    fsync(STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void nb_runtime_init(void){
    signal(SIGPIPE, SIG_IGN);
    {
        int fatal_sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = nb_fatal_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        for(size_t i=0;i<sizeof(fatal_sigs)/sizeof(fatal_sigs[0]);i++){
            if(sigaction(fatal_sigs[i], &sa, NULL) != 0){
                log4c_warn("runtime: sigaction(%s) fail: %s", nb_sig_name(fatal_sigs[i]), strerror(errno));
            }
        }
    }
    {
        struct rlimit rl;
        if(getrlimit(RLIMIT_CORE, &rl) == 0 && rl.rlim_cur == 0){
            rl.rlim_cur = rl.rlim_max;
            if(setrlimit(RLIMIT_CORE, &rl) == 0){
                log4c_info("runtime: core dump enabled soft=%llu hard=%llu",
                    (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
            } else {
                log4c_warn("runtime: setrlimit(RLIMIT_CORE) fail: %s", strerror(errno));
            }
        }
    }
}

static proxy_stream_t* ps_alloc(void){
    int i; for(i=0;i<MAX_CONN;i++) if(!G.streams[i].in_use){
        memset(&G.streams[i],0,sizeof(G.streams[i]));
        G.streams[i].in_use=1; G.streams[i].tcp_fd=-1; G.streams[i].udp_fd=-1; G.streams[i].id=++G.next_ps_id;
        G.streams[i].down_pool_idx=-1;
        G.streams[i].down_pool_id=-1;
        G.streams[i].last_active=picoquic_current_time();
        G.streams[i].created_at=G.streams[i].last_active;
        nb_udp_reassembly_init(&G.streams[i].udp_reassembly);
        g_ps_inuse++; if(g_ps_inuse>g_ps_peak) g_ps_peak=g_ps_inuse;
        return &G.streams[i]; }
    return NULL;
}
static void ps_touch(proxy_stream_t* p){ p->last_active=picoquic_current_time(); }
static proxy_stream_t* ps_find_by_fec_session(uint32_t sid){
    proxy_stream_t* p=fec_session_index_find(sid);
    return p!=NULL&&p->in_use?p:NULL;
}
static proxy_stream_t* ps_find_by_up(picoquic_cnx_t* cnx, uint64_t sid){
    int i; for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
        if(p->in_use && p->up_cnx==cnx && p->up_stream_id==sid) return p; }
    return NULL;
}
static proxy_stream_t* ps_find_by_down(picoquic_cnx_t* cnx, uint64_t sid){
    int i; for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
        if(p->in_use && ((p->down_cnx==cnx && p->down_stream_id==sid) ||
            (p->fec_ctrl_cnx==cnx && p->fec_ctrl_stream_id==sid))) return p; }
    return NULL;
}
static proxy_stream_t* ps_find_by_id(uint32_t id){
    int i; for(i=0;i<MAX_CONN;i++) if(G.streams[i].in_use && G.streams[i].id==id) return &G.streams[i];
    return NULL;
}

static proxy_stream_t* ps_find_udp_up(picoquic_cnx_t* cnx,uint32_t session_id){
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p->in_use&&p->udp_mode&&!p->udp_association&&p->udp_session_id==session_id&&p->up_cnx==cnx)return p;}
    return NULL;
}

static proxy_stream_t* ps_find_udp_down(picoquic_cnx_t* cnx,uint32_t session_id){
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p->in_use&&p->udp_mode&&!p->udp_association&&p->udp_session_id==session_id&&p->down_cnx==cnx)return p;}
    return NULL;
}

static proxy_stream_t* ps_find_udp_child(uint32_t parent_id,const char* host,int port){
    for(int i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];
        if(p->in_use&&p->udp_mode&&!p->udp_association&&p->udp_parent_id==parent_id&&
            p->udp_target_port==port&&!strcasecmp(p->udp_target_host,host))return p;}
    return NULL;
}

static uint32_t udp_session_id_new(void){
    for(uint32_t attempt=0;attempt<MAX_CONN*2u;attempt++){
        uint32_t sid=(uint32_t)picoquic_current_time()^(G.next_ps_id*2654435761u)^attempt;
        if(sid==0)continue;
        int used=0;
        for(int i=0;i<MAX_CONN;i++)if(G.streams[i].in_use&&G.streams[i].udp_session_id==sid){used=1;break;}
        if(!used)return sid;
    }
    return 0;
}

static int sockaddr_ip_equal(const struct sockaddr_storage* a,const struct sockaddr_storage* b){
    if(a==NULL||b==NULL||a->ss_family!=b->ss_family)return 0;
    if(a->ss_family==AF_INET)return ((const struct sockaddr_in*)a)->sin_addr.s_addr==((const struct sockaddr_in*)b)->sin_addr.s_addr;
    if(a->ss_family==AF_INET6)return memcmp(&((const struct sockaddr_in6*)a)->sin6_addr,&((const struct sockaddr_in6*)b)->sin6_addr,16)==0;
    return 0;
}
static void ps_clear_stream_ctx_once(picoquic_cnx_t* cnx, uint64_t sid,
    picoquic_cnx_t** seen_cnx, uint64_t* seen_sid, int* seen_n){
    if(cnx == NULL || seen_cnx == NULL || seen_sid == NULL || seen_n == NULL) return;
    for(int i=0;i<*seen_n;i++){
        if(seen_cnx[i] == cnx && seen_sid[i] == sid) return;
    }
    picoquic_set_app_stream_ctx(cnx, sid, NULL);
    if(*seen_n < 8){
        seen_cnx[*seen_n] = cnx;
        seen_sid[*seen_n] = sid;
        (*seen_n)++;
    }
}
static void ps_free(proxy_stream_t* p){
    if(!p->in_use) return;               /* 幂等: 防拆流/回调重入导致 double free */
    if(p->close_reason[0] == 0) ps_set_close_reason(p, "released");
    if(p->udp_mode && !p->udp_association){
        log4c_info("%s udp flow close id=%u usid=%u reason=%s age=%.1fms packets=%llu/%llu bytes=%llu/%llu milestones_ms=local_c2s:%.1f,c2s_rx:%.1f,c2s_prepare:%.1f,target_ready:%.1f,target_send:%.1f,target_rx:%.1f,s2c_rx:%.1f,s2c_prepare:%.1f,local_s2c:%.1f target=%s:%d route=%s",
            role_name(G.role),p->id,p->udp_session_id,p->close_reason,
            (picoquic_current_time()-p->created_at)/1000.0,
            (unsigned long long)p->udp_packets_c2s,(unsigned long long)p->udp_packets_s2c,
            (unsigned long long)p->bytes_c2s,(unsigned long long)p->bytes_s2c,
            trace_delta_ms(p->udp_first_local_c2s_at,p->created_at),
            trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),
            trace_delta_ms(p->udp_first_c2s_prepare_at,p->created_at),
            trace_delta_ms(p->udp_target_ready_at,p->created_at),
            trace_delta_ms(p->udp_first_target_send_at,p->created_at),
            trace_delta_ms(p->udp_first_target_rx_at,p->created_at),
            trace_delta_ms(p->udp_first_s2c_rx_at,p->created_at),
            trace_delta_ms(p->udp_first_s2c_prepare_at,p->created_at),
            trace_delta_ms(p->udp_first_local_s2c_at,p->created_at),
            p->udp_target_host,p->udp_target_port,p->route);
    }
    if(p->udp_association){
        uint32_t parent_id=p->id;
        for(int i=0;i<MAX_CONN;i++) if(G.streams[i].in_use&&G.streams[i].udp_parent_id==parent_id){
            ps_set_close_reason(&G.streams[i],"udp-association-close");ps_free(&G.streams[i]);
        }
    }
    if(ps_is_rtc_webcast(p)){
        uint64_t now = picoquic_current_time();
        double first_c2s_ms = p->first_c2s_at ? (p->first_c2s_at - p->created_at) / 1000.0 : -1.0;
        double first_s2c_ms = p->first_s2c_at ? (p->first_s2c_at - p->created_at) / 1000.0 : -1.0;
        double connect_ms = p->target_connect_at && p->target_connect_done_at
            ? (p->target_connect_done_at - p->target_connect_at) / 1000.0 : -1.0;
        log4c_info("%s media flow close id=%u reason=%s age=%.1fms c2s=%llu s2c=%llu first_c2s=%.1fms first_s2c=%.1fms target_connect=%s connect_ms=%.1f route=%s",
            role_name(G.role), p->id, p->close_reason, (now - p->created_at) / 1000.0,
            (unsigned long long)p->bytes_c2s, (unsigned long long)p->bytes_s2c,
            first_c2s_ms, first_s2c_ms, ps_connect_state_name(p->target_connect_state),
            connect_ms, p->route);
    }
    log4c_debug("ps free id=%u up_sid=%llu down_sid=%llu route=%s", p->id,
        (unsigned long long)p->up_stream_id, (unsigned long long)p->down_stream_id, p->route);
    picoquic_cnx_t* seen_cnx[8];
    uint64_t seen_sid[8];
    int seen_n = 0;
    if(p->up_cnx) ps_clear_stream_ctx_once(p->up_cnx, p->up_stream_id, seen_cnx, seen_sid, &seen_n);
    if(p->down_cnx && p->down_opened) ps_clear_stream_ctx_once(p->down_cnx, p->down_stream_id, seen_cnx, seen_sid, &seen_n);
    if(p->fec_ctrl_cnx && p->fec_ctrl_opened) ps_clear_stream_ctx_once(p->fec_ctrl_cnx, p->fec_ctrl_stream_id, seen_cnx, seen_sid, &seen_n);
    if(p->tcp_fd>=0){ if(G.epoll_fd>=0)epoll_ctl(G.epoll_fd,EPOLL_CTL_DEL,p->tcp_fd,NULL); close(p->tcp_fd); }
    if(p->udp_fd>=0){ if(G.epoll_fd>=0)epoll_ctl(G.epoll_fd,EPOLL_CTL_DEL,p->udp_fd,NULL); close(p->udp_fd); }
    nb_ring_dispose(&p->down_tx);
    nb_ring_dispose(&p->up_tx);
    free(p->fec_dg_tx); p->fec_dg_tx=NULL;
    free(p->fec_stage_tx); p->fec_stage_tx=NULL;
    free(p->udp_down_tx);p->udp_down_tx=NULL;
    free(p->udp_up_tx);p->udp_up_tx=NULL;
    free(p->udp_pending_tx);p->udp_pending_tx=NULL;
    nb_udp_reassembly_dispose(&p->udp_reassembly);
    fec_session_index_remove(p->fec_session_id,p);
    if(p->fec_engine!=NULL) fec_metrics_add(&g_fec_metrics_done,nb_fec_metrics(p->fec_engine));
    nb_fec_session_destroy(p->fec_engine); p->fec_engine=NULL;
    nb_ring_dispose(&p->q2t);
    if(g_ps_inuse>0) g_ps_inuse--;
    p->in_use=0;
}
/* 主动拆流: discard 两侧 QUIC stream(=reset+stop_sending), 连锁通知上/下游一并回收, 再释放本地。
 * 用于"本地 TCP 端已关闭 / 空闲超时"等一侧先结束的场景, 不再死等对端 FIN。 */
static void ps_teardown(proxy_stream_t* p){
    if(!p->in_use) return;
    if(p->down_cnx && p->down_opened) picoquic_set_app_stream_ctx(p->down_cnx, p->down_stream_id, NULL);
    if(p->up_cnx) picoquic_set_app_stream_ctx(p->up_cnx, p->up_stream_id, NULL);
    if(p->fec_ctrl_cnx && p->fec_ctrl_opened) picoquic_set_app_stream_ctx(p->fec_ctrl_cnx, p->fec_ctrl_stream_id, NULL);
    if(p->down_cnx && p->down_opened) picoquic_discard_stream(p->down_cnx, p->down_stream_id, 0);
    if(p->up_cnx) picoquic_discard_stream(p->up_cnx, p->up_stream_id, 0);
    ps_free(p);
}

static void ps_teardown_reason(proxy_stream_t* p, const char* reason){
    ps_set_close_reason(p, reason);
    ps_teardown(p);
}

/* q2t 动态追加(永不丢数据; 背压靠 QUIC connection flow control 限总量) */
static int q2t_append(proxy_stream_t* p, const uint8_t* d, size_t n){
    if(nb_ring_append(&p->q2t,d,n,NB_Q2T_MAX)!=0){
        log4c_warn("id=%u q2t over limit %zu+%zu > %d",p->id,p->q2t.len,n,NB_Q2T_MAX);return -1;
    }
    if(n>0 && p->first_q2t_queue_at==0) p->first_q2t_queue_at=picoquic_current_time();
    return 0;
}

static int append_bytes(uint8_t** buf, size_t* len, size_t* cap, size_t limit, const uint8_t* d, size_t n, uint32_t ps_id, const char* tag){
    if(n == 0) return 0;
    if(*len + n > limit){
        log4c_warn("id=%u %s over limit %zu+%zu > %zu", ps_id, tag, *len, n, limit);
        return -1;
    }
    if(*len + n > *cap){
        size_t ncap = *cap ? (*cap * 2) : 4096;
        while(ncap < *len + n) ncap *= 2;
        uint8_t* nb = realloc(*buf, ncap);
        if(nb == NULL){
            log4c_error("id=%u %s realloc %zu fail", ps_id, tag, ncap);
            return -1;
        }
        *buf = nb; *cap = ncap;
    }
    memcpy(*buf + *len, d, n);
    *len += n;
    return 0;
}

static void nb_put_u16(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)(v&0xFF); }
static void nb_put_u32(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)(v&0xFF); }
static uint16_t nb_get_u16(const uint8_t* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint32_t nb_get_u32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static int dgramq_append(uint8_t** buf, size_t* len, size_t* cap, const uint8_t* d, size_t n, uint32_t ps_id){
    size_t need = 2 + n;
    if(*len + need > NB_DGRAM_QUEUE_MAX){
        log4c_warn("id=%u fec datagram queue over limit %zu+%zu > %d", ps_id, *len, need, NB_DGRAM_QUEUE_MAX);
        return -1;
    }
    if(*len + need > *cap){
        size_t ncap = *cap ? (*cap * 2) : 4096;
        while(ncap < *len + need) ncap *= 2;
        uint8_t* nb = realloc(*buf, ncap);
        if(nb == NULL){
            log4c_error("id=%u fec datagram queue realloc %zu fail", ps_id, ncap);
            return -1;
        }
        *buf = nb; *cap = ncap;
    }
    nb_put_u16(*buf + *len, (uint16_t)n);
    memcpy(*buf + *len + 2, d, n);
    *len += need;
    return 0;
}

static int dgramq_peek(uint8_t* buf, size_t len, const uint8_t** d, size_t* n){
    if(len < 2) return 0;
    *n = nb_get_u16(buf);
    if(len < 2 + *n) return 0;
    *d = buf + 2;
    return 1;
}

static void dgramq_consume(uint8_t* buf, size_t* len){
    const uint8_t* d=NULL; size_t n=0;
    if(!dgramq_peek(buf,*len,&d,&n)) return;
    memmove(buf, buf + 2 + n, *len - (2 + n));
    *len -= (2 + n);
}

static int udp_queue_wire(proxy_stream_t* p,int to_down,const uint8_t* data,size_t length){
    uint8_t** buf=to_down?&p->udp_down_tx:&p->udp_up_tx;
    size_t* len=to_down?&p->udp_down_tx_len:&p->udp_up_tx_len;
    size_t* cap=to_down?&p->udp_down_tx_cap:&p->udp_up_tx_cap;
    picoquic_cnx_t* cnx=to_down?p->down_cnx:p->up_cnx;
    if(cnx==NULL||dgramq_append(buf,len,cap,data,length,p->id)!=0)return -1;
    if(picoquic_mark_datagram_ready(cnx,1)!=0)return -1;
    return 0;
}

static int udp_queue_packet(proxy_stream_t* p,int to_down,uint8_t type,const char* route,
    const uint8_t* payload,size_t payload_length){
    uint16_t fragments=nb_udp_fragment_count(payload_length);
    size_t route_length=route?strlen(route):0;
    if(fragments==0||route_length==0||route_length>=NB_UDP_ROUTE_MAX)return -1;
    uint32_t sequence=++p->udp_tx_sequence;
    for(uint16_t i=0;i<fragments;i++){
        size_t offset=(size_t)i*NB_UDP_FRAGMENT_PAYLOAD;
        size_t part=payload_length-offset;if(part>NB_UDP_FRAGMENT_PAYLOAD)part=NB_UDP_FRAGMENT_PAYLOAD;
        uint8_t wire[NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32];
        int n=nb_udp_wire_encode(wire,sizeof(wire),type,p->udp_session_id,sequence,i,fragments,
            (uint16_t)payload_length,route,(uint16_t)route_length,payload+offset,(uint16_t)part);
        if(n<0||udp_queue_wire(p,to_down,wire,(size_t)n)!=0)return -1;
    }
    return 0;
}

static int udp_forward_fragment(proxy_stream_t* p,int to_down,const nb_udp_wire_view_t* view,
    const char* route){
    size_t route_length=strlen(route);uint8_t wire[NB_UDP_FRAGMENT_PAYLOAD+NB_UDP_ROUTE_MAX+32];
    int n=nb_udp_wire_encode(wire,sizeof(wire),view->type,view->session_id,view->sequence,
        view->fragment_index,view->fragment_count,view->total_length,route,(uint16_t)route_length,
        view->payload,view->payload_length);
    return n<0?-1:udp_queue_wire(p,to_down,wire,(size_t)n);
}

static int fec_queue_datagram(proxy_stream_t* p, const uint8_t* d, size_t n){
    if(p->fec_dg_cnx == NULL) return -1;
    if(dgramq_append(&p->fec_dg_tx, &p->fec_dg_tx_len, &p->fec_dg_tx_cap, d, n, p->id) != 0){
        p->need_teardown = 1;
        return -1;
    }
    if(picoquic_mark_datagram_ready(p->fec_dg_cnx, 1) != 0){
        log4c_warn("id=%u mark datagram ready fail", p->id);
    }
    return 0;
}

static int fec_stage_bytes(proxy_stream_t* p, const uint8_t* d, size_t n, int fin){
    if(append_bytes(&p->fec_stage_tx, &p->fec_stage_tx_len, &p->fec_stage_tx_cap, NB_QTX_MAX, d, n, p->id, "fec-stage") != 0){
        p->need_teardown = 1;
        return -1;
    }
    if(fin) p->fec_stage_tx_fin = 1;
    return 0;
}

static int fec_ctrl_send(proxy_stream_t* p, const uint8_t* d, size_t n){
    if((p->fec_ctrl_cnx == p->up_cnx && p->fec_ctrl_stream_id == p->up_stream_id) || G.role == ROLE_EXIT){
        return queue_up(p, d, n, 0);
    }
    return queue_down(p, d, n, 0);
}

static int fec_engine_send_datagram(void* ctx, const uint8_t* data, size_t len){
    proxy_stream_t* p=(proxy_stream_t*)ctx;
    if(p==NULL||!p->in_use) return -1;
    if(fec_queue_datagram(p,data,len)!=0) return -1;
    p->fec_dg_sent++;
    return 0;
}

static int fec_engine_send_control(void* ctx, const uint8_t* data, size_t len){
    proxy_stream_t* p=(proxy_stream_t*)ctx;
    return (p!=NULL&&p->in_use)?fec_ctrl_send(p,data,len):-1;
}

static int fec_engine_deliver(void* ctx, const uint8_t* data, size_t len, int fin){
    proxy_stream_t* p=(proxy_stream_t*)ctx;
    if(p==NULL||!p->in_use) return -1;
    if(G.role==ROLE_EXIT){
        if(q2t_append(p,data,len)!=0){ p->need_teardown=1; return -1; }
        if(fin) p->q2t_fin=1;
    }else if(p->fec_rx_to_down){
        fwd_down(p,(uint8_t*)data,len,fin);
    }else{
        fwd_up(p,(uint8_t*)data,len,fin);
    }
    return p->need_teardown?-1:0;
}

static int fec_engine_ensure(proxy_stream_t* p){
    if(p->fec_engine!=NULL) return 0;
    if(fec_session_index_add(p->fec_session_id,p)!=0){
        log4c_error("id=%u duplicate fec session sid=%u",p->id,p->fec_session_id);return -1;
    }
    nb_fec_callbacks_t cb={fec_engine_send_datagram,fec_engine_send_control,fec_engine_deliver};
    p->fec_engine=nb_fec_session_create(p->fec_session_id,&g_fec_cfg,&cb,p);
    if(p->fec_engine==NULL){
        log4c_error("id=%u fec engine create fail sid=%u",p->id,p->fec_session_id);
        fec_session_index_remove(p->fec_session_id,p);return -1;
    }
    return 0;
}

static int fec_build_datagram(uint8_t* out, size_t cap, uint8_t type, uint32_t session_id, uint32_t seq){
    if(cap < 14) return -1;
    nb_put_u32(out, NB_FEC_DGRAM_MAGIC);
    out[4] = NB_FEC_DGRAM_VER;
    out[5] = type;
    nb_put_u32(out + 6, session_id);
    nb_put_u32(out + 10, seq);
    return 14;
}

static int fec_queue_probe_datagram(proxy_stream_t* p, uint8_t type){
    uint8_t buf[32];
    int n = fec_build_datagram(buf, sizeof(buf), type, p->fec_session_id, (uint32_t)(++p->fec_dg_sent));
    return (n > 0) ? fec_queue_datagram(p, buf, (size_t)n) : -1;
}

static int fec_should_drop(uint8_t type, uint32_t block_id, uint8_t symbol_idx){
    (void)block_id;
    if(type == nb_fec_dgram_source && g_fec_v15_drop_src_mod > 0){
        uint64_t seq = (uint64_t)block_id * NB_FEC_V15_K + symbol_idx + 1;
        if((seq % g_fec_v15_drop_src_mod) == 0){ g_fec_v15_drop_src++; return 1; }
    } else if(type == nb_fec_dgram_repair && g_fec_v15_drop_repair_mod > 0){
        uint64_t seq = (uint64_t)block_id + symbol_idx + 1;
        if((seq % g_fec_v15_drop_repair_mod) == 0){ g_fec_v15_drop_repair++; return 1; }
    }
    return 0;
}

static void fec_ctrl_handle_line(proxy_stream_t* p, const char* line, int from_down){
    uint32_t sid=0; int prio=0; char route[300];
    if(strncmp(line, NB_FEC_CTRL_START ":", strlen(NB_FEC_CTRL_START ":")) == 0){
        route[0]=0;
        if(sscanf(line, "FC:START:%u:%d:%299[^\n]", &sid, &prio, route) >= 2){
            p->fec_ctrl_only = 1;
            p->fec_sidecar_mode = 1;
            p->fec_session_id = sid;
            p->prio = prio;
            if(route[0]) snprintf(p->fec_route, sizeof(p->fec_route), "%s", route);
            p->fec_dg_cnx = p->up_cnx;
            p->fec_ctrl_cnx = p->up_cnx;
            p->fec_ctrl_stream_id = p->up_stream_id;
            p->fec_ctrl_opened = 1;
            if(fec_engine_ensure(p)!=0){ p->need_teardown=1; return; }
            log4c_info("%s fec ctrl START sid=%u prio=%d route=%s", role_name(G.role), sid, prio, p->fec_route);
            if(G.role == ROLE_MIDDLE && !from_down && route[0]){
                char first[300], rest[300]; rest[0]=0;
                char* comma=strchr(route, ',');
                if(comma){
                    size_t fl=(size_t)(comma-route); if(fl>=sizeof(first)) fl=sizeof(first)-1;
                    memcpy(first, route, fl); first[fl]=0; snprintf(rest, sizeof(rest), "%s", comma+1);
                } else {
                    snprintf(first, sizeof(first), "%s", route);
                }
                if(strncmp(first,"H:",2)==0){
                    char nhost[256]; int nport=0; char* c=strrchr(first+2,':');
                    if(c){
                        size_t host_len=(size_t)(c-(first+2));
                        if(host_len==0||host_len>=sizeof(nhost)||parse_port_strict(c+1,&nport)!=0){ps_teardown(p);return;}
                        memcpy(nhost,first+2,host_len);nhost[host_len]=0;
                        struct sockaddr_storage naddr; int is_name=0;
                        nb_flow_policy_t pol;
                        char thost[256]; int tport=0;
                        if(parse_target_from_route(rest[0]?rest:"", thost, sizeof(thost), &tport) == 0) flow_policy_for_host(thost, tport, &pol);
                        else nb_flow_policy_default(&pol);
                        if(!pol.matched) pol.prio = prio;
                        if(picoquic_get_server_address(nhost,nport,&naddr,&is_name)==0){
                            p->down_pool_id=pool_get_or_create(&naddr);
                            if(p->down_pool_id<0){ps_teardown(p);return;}
                            if(downstream_open_stream(p, rest[0]?rest:"", &pol)!=0){ ps_teardown(p); return; }
                            p->fec_rx_to_down = 1;
                        }
                    }
                }
            }
            if(G.role == ROLE_EXIT && p->tcp_fd < 0 && route[0]){
                char thost[256]; int tport=0;
                if(fec_parse_target_route(route, thost, sizeof(thost), &tport) == 0){
                    if(!whitelist_allowed(thost,tport)){
                        log4c_debug("exit id=%u fec ctrl BLOCKED %s:%d (not in whitelist)",p->id,thost,tport);
                        ps_teardown(p);
                        return;
                    }
                    if(dns_submit(p->id,thost,tport)!=0){
                        log4c_error("id=%u fec ctrl dns queue full %s:%d",p->id,thost,tport);
                        ps_teardown(p);
                        return;
                    }
                    p->dns_pending=1; p->target_port=tport;
                }
            }
            if(G.role == ROLE_EXIT && !from_down){
                char ack[64];
                int al = snprintf(ack, sizeof(ack), NB_FEC_CTRL_ACK ":%u\n", sid);
                if(queue_up(p, (const uint8_t*)ack, (size_t)al, 0) != 0){
                    log4c_warn("exit fec ctrl ack queue fail sid=%u", sid);
                }
            }
        }
    } else if(sscanf(line, "FC:ACK:%u", &sid) == 1){
        proxy_stream_t* s = from_down ? p : ps_find_by_fec_session(sid);
        if(s != NULL){
            s->fec_ctrl_peer_ready = 1;
            s->fec_sidecar_mode = 1;
            log4c_info("%s fec ctrl ACK sid=%u", role_name(G.role), sid);
            if(G.role == ROLE_MIDDLE && from_down){
                if((s->fec_stage_tx_len > 0 || s->fec_stage_tx_fin) && !s->need_teardown){
                    if(nb_fec_tx_feed(s->fec_engine, s->fec_stage_tx, s->fec_stage_tx_len,
                        s->fec_stage_tx_fin, picoquic_current_time()) != 0){
                        s->need_teardown = 1;
                    }
                    s->fec_stage_tx_len = 0;
                    s->fec_stage_tx_fin = 0;
                }
                (void)fec_queue_probe_datagram(s, nb_fec_dgram_hello);
            }
        }
    } else if(p->fec_engine!=NULL){
        int rc=nb_fec_on_control(p->fec_engine,line,strlen(line),picoquic_current_time());
        if(rc!=NB_FEC_OK){
            log4c_warn("%s fec engine control fail sid=%u rc=%d line=%.32s",role_name(G.role),p->fec_session_id,rc,line);
            p->need_teardown=1;
        }
        return;
    } else {
        log4c_warn("%s fec control without engine: %.32s",role_name(G.role),line);
        p->need_teardown=1;
    }
}

static void fec_ctrl_feed_bytes(proxy_stream_t* p, uint8_t* data, size_t n, int from_down){
    if(n == 0) return;
    size_t cp = n;
    if(p->fec_ctrl_rxlen + cp >= sizeof(p->fec_ctrl_rxbuf)) cp = sizeof(p->fec_ctrl_rxbuf) - p->fec_ctrl_rxlen - 1;
    memcpy(p->fec_ctrl_rxbuf + p->fec_ctrl_rxlen, data, cp);
    p->fec_ctrl_rxlen += cp;
    p->fec_ctrl_rxbuf[p->fec_ctrl_rxlen] = 0;
    char* nl;
    while((nl = strchr(p->fec_ctrl_rxbuf, '\n')) != NULL){
        *nl = 0;
        fec_ctrl_handle_line(p, p->fec_ctrl_rxbuf, from_down);
        size_t used = (size_t)(nl - p->fec_ctrl_rxbuf + 1);
        memmove(p->fec_ctrl_rxbuf, p->fec_ctrl_rxbuf + used, p->fec_ctrl_rxlen - used);
        p->fec_ctrl_rxlen -= used;
        p->fec_ctrl_rxbuf[p->fec_ctrl_rxlen] = 0;
    }
}

static int fec_parse_target_route(const char* route, char* host, size_t host_cap, int* port){
    if(strncmp(route, "T:", 2) != 0) return -1;
    char tmp[300];
    size_t len=strlen(route+2);if(len==0||len>=sizeof(tmp))return -1;memcpy(tmp,route+2,len+1);
    char* c = strrchr(tmp, ':');
    if(c == NULL) return -1;
    *c = 0;
    size_t host_len=strlen(tmp);if(host_len==0||host_len>=host_cap)return -1;memcpy(host,tmp,host_len+1);
    return parse_port_strict(c+1,port);
}

static int queue_down(proxy_stream_t* p, const uint8_t* d, size_t n, int fin){
    if(nb_ring_append(&p->down_tx,d,n,NB_QTX_MAX)!=0){
        p->need_teardown = 1;
        return -1;
    }
    if(n>0 && p->first_down_queue_at==0){
        p->first_down_queue_at=picoquic_current_time();
        if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=c2s-queue age=%.1fms bytes=%zu q=%zu route=%s",
            role_name(G.role),p->id,trace_delta_ms(p->first_down_queue_at,p->created_at),n,p->down_tx.len,p->route);
    }
    if(fin) p->down_tx_fin = 1;
    if(p->down_cnx && p->down_opened){
        picoquic_mark_active_stream(p->down_cnx, p->down_stream_id, 1, p);
    }
    return 0;
}

static int queue_down_prefix(proxy_stream_t* p, const uint8_t* d, size_t n){
    if(p==NULL||d==NULL||n==0||n>sizeof(p->down_prefix)||p->down_prefix_off<p->down_prefix_len){
        if(p!=NULL)p->need_teardown=1;
        return -1;
    }
    memcpy(p->down_prefix,d,n);
    p->down_prefix_len=n;
    p->down_prefix_off=0;
    if(p->down_prefix_queued_at==0) p->down_prefix_queued_at=picoquic_current_time();
    if(p->down_cnx&&p->down_opened){
        picoquic_mark_active_stream(p->down_cnx,p->down_stream_id,1,p);
    }
    return 0;
}

static int queue_up(proxy_stream_t* p, const uint8_t* d, size_t n, int fin){
    if(nb_ring_append(&p->up_tx,d,n,NB_QTX_MAX)!=0){
        p->need_teardown = 1;
        return -1;
    }
    if(n>0 && p->first_up_queue_at==0){
        p->first_up_queue_at=picoquic_current_time();
        if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=s2c-queue age=%.1fms bytes=%zu q=%zu route=%s",
            role_name(G.role),p->id,trace_delta_ms(p->first_up_queue_at,p->created_at),n,p->up_tx.len,p->route);
    }
    if(fin) p->up_tx_fin = 1;
    if(p->up_cnx){
        picoquic_mark_active_stream(p->up_cnx, p->up_stream_id, 1, p);
    }
    return 0;
}

/* 非阻塞 connect 目标 TCP(exit): 用已(异步)解析好的地址, 不在事件循环内做阻塞 DNS */
static int tcp_connect_addr(struct sockaddr* sa, socklen_t sl){
    int fd=socket(sa->sa_family,SOCK_STREAM,0);
    if(fd<0) return -1;
    set_nonblock(fd); int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    if(G.outbound_configured){
        if(G.outbound_addr.ss_family!=sa->sa_family){close(fd);errno=EAFNOSUPPORT;return -1;}
        socklen_t source_len=sa->sa_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
        if(bind(fd,(struct sockaddr*)&G.outbound_addr,source_len)!=0){close(fd);return -1;}
    }
    if(connect(fd,sa,sl)<0 && errno!=EINPROGRESS){ close(fd); return -1; }
    return fd;
}

static int udp_connect_addr(struct sockaddr* sa,socklen_t sl){
    int fd=socket(sa->sa_family,SOCK_DGRAM,0);if(fd<0)return -1;
    if(set_nonblock(fd)!=0){close(fd);return -1;}
    if(G.outbound_configured){
        if(G.outbound_addr.ss_family!=sa->sa_family){close(fd);errno=EAFNOSUPPORT;return -1;}
        socklen_t source_len=sa->sa_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
        if(bind(fd,(struct sockaddr*)&G.outbound_addr,source_len)!=0){close(fd);return -1;}
    }
    if(connect(fd,sa,sl)!=0){close(fd);return -1;}return fd;
}

/* 前置声明 */
static int relay_quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t ev, void* cb_ctx, void* v_stream_ctx);
static int epoll_udp_update(proxy_stream_t* p,int add);
static void udp_local_drain(proxy_stream_t* p);

/* 确保池中 idx 槽有一条已启动的连接(空槽则新建 + start + 保活)。返回 0=可用 -1=失败 */
static int pool_ensure(cnx_pool_t* pool, int idx){
    if(pool->cnx[idx]!=NULL) return 0;
    picoquic_cnx_t* c=picoquic_create_cnx(G.quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&pool->addr, picoquic_current_time(), 0, NB_SNI, NB_ALPN, 1);
    if(c==NULL){ log4c_error("pool[%d] create cnx fail",idx); return -1; }
    if((G.role==ROLE_ENTRY || G.role==ROLE_MIDDLE) &&
        picoquic_set_loss_reorder_tolerance(c,g_reorder_gap,g_reorder_delay_us)!=0){
        log4c_error("pool[%d] invalid reorder tolerance gap=%llu delay=%lluus",idx,
            (unsigned long long)g_reorder_gap,(unsigned long long)g_reorder_delay_us);
        picoquic_delete_cnx(c); return -1;
    }
    picoquic_set_callback(c, relay_quic_callback, NULL);
    if(picoquic_start_client_cnx(c)!=0){ log4c_error("pool[%d] start cnx fail",idx); return -1; }
    picoquic_enable_keep_alive(c, 15000000); /* 15s 保活 */
    pool->cnx[idx]=c; pool->next_sid[idx]=0;
    return 0;
}

/* 初始化连接池(配下一跳地址 + 建满 POOL_SIZE 条)。返回 0=至少一条就绪 -1=全失败 */
static int pool_init(cnx_pool_t* pool, struct sockaddr_storage* addr){
    pool->addr=*addr; pool->rr=0; pool->rr_lat=0;
    memset(pool->recent_loss, 0, sizeof(pool->recent_loss));
    memset(pool->recent_rtt, 0, sizeof(pool->recent_rtt));
    memset(pool->recent_rtt_max, 0, sizeof(pool->recent_rtt_max));
    memset(pool->recent_sent, 0, sizeof(pool->recent_sent));
    memset(pool->last_sent_total, 0, sizeof(pool->last_sent_total));
    memset(pool->last_lost_total, 0, sizeof(pool->last_lost_total));
    memset(pool->last_timer_total, 0, sizeof(pool->last_timer_total));
    memset(pool->last_spurious_total, 0, sizeof(pool->last_spurious_total));
    memset(pool->last_retrans_total, 0, sizeof(pool->last_retrans_total));
    memset(pool->last_preempt_total, 0, sizeof(pool->last_preempt_total));
    memset(pool->recent_rtt_var, 0, sizeof(pool->recent_rtt_var));
    memset(pool->recent_ts, 0, sizeof(pool->recent_ts));
    pool->fec_latched = 0;
    int ok=0; for(int i=0;i<POOL_SIZE;i++) if(pool_ensure(pool,i)==0) ok++;
    pool->configured=1;
    log4c_info("cnx pool ready: %d/%d connections up",ok,POOL_SIZE);
    return ok>0?0:-1;
}

static int pool_endpoint_equal(const struct sockaddr_storage* a,const struct sockaddr_storage* b){
    if(a->ss_family!=b->ss_family)return 0;
    if(a->ss_family==AF_INET){
        const struct sockaddr_in* x=(const struct sockaddr_in*)a;const struct sockaddr_in* y=(const struct sockaddr_in*)b;
        return x->sin_port==y->sin_port&&x->sin_addr.s_addr==y->sin_addr.s_addr;
    }
    if(a->ss_family==AF_INET6){
        const struct sockaddr_in6* x=(const struct sockaddr_in6*)a;const struct sockaddr_in6* y=(const struct sockaddr_in6*)b;
        return x->sin6_port==y->sin6_port&&memcmp(&x->sin6_addr,&y->sin6_addr,sizeof(x->sin6_addr))==0;
    }
    return 0;
}

static int pool_contains_cnx(picoquic_cnx_t* cnx){
    if(cnx==NULL)return 0;
    for(int pool_id=0;pool_id<G.pool_count;pool_id++){
        for(int idx=0;idx<POOL_SIZE;idx++){
            if(G.pools[pool_id].cnx[idx]==cnx)return 1;
        }
    }
    return 0;
}

static int pool_get_or_create(struct sockaddr_storage* addr){
    for(int i=0;i<G.pool_count;i++)if(G.pools[i].configured&&pool_endpoint_equal(&G.pools[i].addr,addr))return i;
    if(G.pool_count>=NB_MAX_POOLS){log4c_error("next-hop pool registry full max=%d",NB_MAX_POOLS);return -1;}
    int id=G.pool_count;
    if(pool_init(&G.pools[id],addr)!=0)return -1;
    G.pool_count++;
    log4c_info("next-hop pool registered id=%d",id);
    return id;
}

static int pool_pick_all_rr(cnx_pool_t* pool, int avoid_idx){
    for(int k=0;k<POOL_SIZE;k++){
        int idx=pool->rr_lat%POOL_SIZE;pool->rr_lat++;
        if(idx==avoid_idx) continue;
        if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
        if(pool->cnx[idx]!=NULL) return idx;
    }
    return -1;
}

#define FEC_LOSS_ON_PCT 3.0
#define FEC_LOSS_OFF_PCT 1.0
#define FEC_JITTER_ON_US 40000ULL
#define FEC_JITTER_OFF_US 15000ULL
#define FEC_MIN_SENT_PKTS 20ULL
#define FEC_COLDSTART_US 3000000ULL
#define REALTIME_LOSS_PRIOR_PCT 2.0
#define REALTIME_HEALTHY_LOSS_PCT 3.0
#define REALTIME_HEALTHY_JITTER_US 40000ULL

static int pool_quality_fresh(cnx_pool_t* pool, int idx, uint64_t now){
    return pool->recent_ts[idx] != 0 && now >= pool->recent_ts[idx] &&
        now - pool->recent_ts[idx] <= 30000000ULL; /* 30s 内样本视为新鲜 */
}

static int pool_quality_sampled(cnx_pool_t* pool, int idx, uint64_t now){
    return pool_quality_fresh(pool, idx, now) && pool->recent_sent[idx] >= FEC_MIN_SENT_PKTS;
}

static double pool_quality_loss_score(cnx_pool_t* pool, int idx){
    double confidence=(double)pool->recent_sent[idx]/(double)FEC_MIN_SENT_PKTS;
    if(confidence>1.0)confidence=1.0;
    return pool->recent_loss[idx]*confidence+REALTIME_LOSS_PRIOR_PCT*(1.0-confidence);
}

static int pool_quality_better(cnx_pool_t* pool, int a, int b, uint64_t now){
    int af = pool_quality_fresh(pool, a, now), bf = pool_quality_fresh(pool, b, now);
    if(af != bf) return af > bf;
    if(!af && !bf) return a < b;
    double as=pool_quality_loss_score(pool,a),bs=pool_quality_loss_score(pool,b);
    if(as != bs) return as < bs;
    if(pool->recent_rtt_var[a] != pool->recent_rtt_var[b]) return pool->recent_rtt_var[a] < pool->recent_rtt_var[b];
    if(pool->recent_rtt[a] != pool->recent_rtt[b]) return pool->recent_rtt[a] < pool->recent_rtt[b];
    return a < b;
}

static int pool_quality_best(cnx_pool_t* pool, int avoid_idx, int require_sampled){
    uint64_t now = picoquic_current_time();
    int best = -1;
    for(int idx=0; idx<POOL_SIZE; idx++){
        if(idx==avoid_idx) continue;
        if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
        if(pool->cnx[idx]==NULL) continue;
        if(require_sampled){
            if(!pool_quality_sampled(pool, idx, now)) continue;
        } else {
            if(!pool_quality_fresh(pool, idx, now)) continue;
        }
        if(best<0 || pool_quality_better(pool, idx, best, now)) best = idx;
    }
    return best;
}

/* Q2: 要不要双发？看这条 hop 最近一个采样窗的线质是否变差。
 * 使用"增量 loss + 回滞"版:
 * - 只在样本足够(delta sent >= FEC_MIN_SENT_PKTS)时才更新判断
 * - 打开阈值高于关闭阈值, 避免在边缘抖动时来回开关
 * - 先以全池最优连接的增量 loss/jitter 作为该 hop 当前代表值 */
static int pool_line_bad(cnx_pool_t* pool, int prio, double* best_loss, double* best_jitter_ms, int* best_idx, int* coldstart_on){
    int idx = pool_quality_best(pool, -1, 1);
    int idx2 = (idx >= 0) ? pool_quality_best(pool, idx, 1) : -1;
    if(coldstart_on) *coldstart_on = 0;
    if(best_idx) *best_idx = (idx2 >= 0) ? idx2 : idx;
    if(idx < 0){
        if(best_loss) *best_loss = -1.0;
        if(best_jitter_ms) *best_jitter_ms = -1.0;
        if(prio_is_fec_candidate(prio)){
            int fresh_idx = pool_quality_best(pool, -1, 0);
            if(fresh_idx >= 0 && pool->recent_ts[fresh_idx] != 0 &&
                picoquic_current_time() - pool->recent_ts[fresh_idx] <= FEC_COLDSTART_US){
                if(coldstart_on) *coldstart_on = 1;
                pool->fec_latched = 1;
                if(best_idx) *best_idx = fresh_idx;
                if(best_loss) *best_loss = pool->recent_loss[fresh_idx];
                if(best_jitter_ms) *best_jitter_ms = (double)pool->recent_rtt_var[fresh_idx] / 1000.0;
                return 1;
            }
        }
        return pool->fec_latched;
    }
    int eval_idx = (idx2 >= 0) ? idx2 : idx; /* 用前两条最优里的第二优值, 比单看最优更保守 */
    double loss = pool->recent_loss[eval_idx];
    double jitter_ms = (double)pool->recent_rtt_var[eval_idx] / 1000.0;
    if(best_loss) *best_loss = loss;
    if(best_jitter_ms) *best_jitter_ms = jitter_ms;
    if(idx2 < 0){
        /* 只有一条样本充足的连接时, 允许它触发打开, 但不允许它单独触发关闭。 */
        if(loss >= FEC_LOSS_ON_PCT || pool->recent_rtt_var[eval_idx] >= FEC_JITTER_ON_US){
            pool->fec_latched = 1;
            return 1;
        }
        if(prio_is_fec_candidate(prio) && pool->recent_ts[eval_idx] != 0 &&
            picoquic_current_time() - pool->recent_ts[eval_idx] <= FEC_COLDSTART_US){
            if(coldstart_on) *coldstart_on = 1;
            pool->fec_latched = 1;
            return 1;
        }
        return pool->fec_latched;
    }
    if(!pool->fec_latched){
        if(loss >= FEC_LOSS_ON_PCT || pool->recent_rtt_var[eval_idx] >= FEC_JITTER_ON_US) pool->fec_latched = 1;
    } else {
        if(loss <= FEC_LOSS_OFF_PCT && pool->recent_rtt_var[eval_idx] <= FEC_JITTER_OFF_US) pool->fec_latched = 0;
    }
    return pool->fec_latched;
}

/* FEC 从全池挑当前最稳连接；优先使用 30s 内有效样本，无样本时回退全池轮询。 */
static int pool_pick_fec_best(cnx_pool_t* pool, int avoid_idx){
    uint64_t now = picoquic_current_time();
    int best = -1;
    for(int idx=0; idx<POOL_SIZE; idx++){
        if(idx==avoid_idx) continue;
        if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
        if(pool->cnx[idx]==NULL) continue;
        if(!pool_quality_sampled(pool, idx, now)) continue;
        if(best<0 || pool_quality_better(pool, idx, best, now)) best = idx;
    }
    if(best>=0) return best;
    best = pool_quality_best(pool, avoid_idx, 0);
    return best>=0 ? best : pool_pick_all_rr(pool, avoid_idx);
}

static int pool_pick_latency_quality(cnx_pool_t* pool){
    int trusted=pool_quality_best(pool,-1,1);
    if(trusted>=0&&pool->recent_loss[trusted]<=REALTIME_HEALTHY_LOSS_PCT&&
        pool->recent_rtt_var[trusted]<=REALTIME_HEALTHY_JITTER_US){
        int second=pool_quality_best(pool,trusted,1);
        if(second>=0&&pool->recent_loss[second]<=REALTIME_HEALTHY_LOSS_PCT&&
            pool_quality_loss_score(pool,second)<=pool_quality_loss_score(pool,trusted)+1.0&&
            pool->recent_rtt_var[second]<=pool->recent_rtt_var[trusted]+10000ULL){
            return (pool->rr_lat++&1)?second:trusted;
        }
        return trusted;
    }
    int best=pool_quality_best(pool,-1,0);
    if(best<0)return pool_pick_all_rr(pool,-1);
    int second=pool_quality_best(pool,best,0);
    if(second>=0&&pool_quality_loss_score(pool,second)<=pool_quality_loss_score(pool,best)+1.0&&
        pool->recent_rtt_var[second]<=pool->recent_rtt_var[best]+10000ULL){
        return (pool->rr_lat++&1)?second:best;
    }
    return best;
}

/* 实时流从全池按 loss -> jitter -> RTT 选路；批量流仍优先后半池。 */
static int pool_pick(cnx_pool_t* pool, int is_latency){
    if(is_latency){
        return pool_pick_latency_quality(pool);
    } else if(POOL_SIZE>BULK_POOL_START){
        for(int k=BULK_POOL_START;k<POOL_SIZE;k++){
            int idx=BULK_POOL_START+(pool->rr % (POOL_SIZE-BULK_POOL_START)); pool->rr++;
            if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
            if(pool->cnx[idx]!=NULL) return idx;
        }
    }
    for(int k=0;k<POOL_SIZE;k++){ if(pool->cnx[k]==NULL) pool_ensure(pool,k); if(pool->cnx[k]!=NULL) return k; }
    return -1;
}

/* 分配下游 stream 并发首部(带 <prio>; 标记端到端传递)+ 设该 stream 优先级。 */
static int downstream_open_stream(proxy_stream_t* p, const char* route, const nb_flow_policy_t* pol){
    if(p->down_pool_id<0||p->down_pool_id>=G.pool_count){log4c_error("id=%u has no downstream pool",p->id);return -1;}
    cnx_pool_t* pool=&G.pools[p->down_pool_id];
    int prio = pol ? pol->prio : p->prio;
    double best_loss=-1.0, best_jitter_ms=-1.0;
    int best_idx=-1;
    int coldstart_on=0;
    int line_bad = ((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) &&
        (g_fec_v15_observe || g_fec_v15_active))
        ? pool_line_bad(pool, prio, &best_loss, &best_jitter_ms, &best_idx, &coldstart_on) : 0;
    int latency_hint = pol ? (pol->lane_hint == NB_FLOW_LANE_LATENCY) : prio_is_latency(prio);
    int v15_prio_ok = pol ? (pol->fec_hint == NB_FLOW_FEC_FORCE_ON || pol->fec_hint == NB_FLOW_FEC_AUTO) : prio_is_fec_candidate(prio);
    int fec_v15_would_enable = (G.role==ROLE_MIDDLE && g_fec_v15_observe && v15_prio_ok &&
        (line_bad || g_fec_v15_force));
    int fec_v15 = (g_fec_v15_active && fec_v15_would_enable);
    if(pol) apply_flow_policy(p, pol);
    if((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) && latency_hint){
        log4c_info("%s id=%u fec gate route=%s class=%s lane=%s fec_hint=%s rule=%s prio=%d line_bad=%d coldstart=%d best=%d best_loss=%.2f best_jitter=%.1fms v15_active=%d v15_would=%d",
            role_name(G.role), p->id, route,
            pol ? nb_flow_class_name(pol->flow_class) : nb_flow_class_name(p->flow_class),
            pol ? nb_flow_lane_name(pol->lane_hint) : nb_flow_lane_name(p->lane_hint),
            pol ? nb_flow_fec_name(pol->fec_hint) : nb_flow_fec_name(p->fec_hint),
            pol ? pol->rule_name : p->flow_rule,
            prio, line_bad, coldstart_on, best_idx, best_loss, best_jitter_ms, fec_v15,
            fec_v15_would_enable);
    }

    int idx = fec_v15 ? pool_pick_fec_best(pool, -1) : pool_pick(pool, latency_hint);
    if(idx<0){ log4c_error("id=%u no downstream cnx in pool", p->id); return -1; }
    if(latency_hint){
        log4c_info("%s id=%u realtime pool select=%d sampled=%d dsent=%llu loss=%.2f score=%.2f jitter=%.1fms rtt=%.1fms",
            role_name(G.role),p->id,idx,pool_quality_sampled(pool,idx,picoquic_current_time()),
            (unsigned long long)pool->recent_sent[idx],pool->recent_loss[idx],pool_quality_loss_score(pool,idx),
            pool->recent_rtt_var[idx]/1000.0,pool->recent_rtt[idx]/1000.0);
    }
    picoquic_cnx_t* c=pool->cnx[idx];
    p->down_cnx=c; p->prio=prio; p->down_pool_idx=idx;
    p->down_stream_id=pool->next_sid[idx]; pool->next_sid[idx]+=4; /* 该连接独立的 client bidi */
    p->downstream_open_at=picoquic_current_time();
    picoquic_set_app_stream_ctx(c,p->down_stream_id,p);
    picoquic_set_stream_priority(c,p->down_stream_id,
        (uint8_t)(latency_hint ? NB_ROUTE_BOOTSTRAP_PRIO : prio));
    if(fec_v15){
        p->fec_sidecar_mode = 1;
        p->fec_session_id=fec_session_id_new();
        if(p->fec_session_id==0){log4c_error("id=%u unable to allocate fec session id",p->id);return -1;}
        p->fec_ctrl_cnx = c;
        p->fec_ctrl_stream_id = p->down_stream_id;
        p->fec_ctrl_opened = 1;
        p->fec_dg_cnx = c;
        snprintf(p->fec_route, sizeof(p->fec_route), "%s", route);
        p->down_opened=1;
        if(fec_engine_ensure(p)!=0) return -1;
        {
            char ctrl[420];
            int cl=snprintf(ctrl,sizeof(ctrl),NB_FEC_CTRL_START ":%u:%d:%s\n",p->fec_session_id,prio,route);
            if(cl<=0||(size_t)cl>=sizeof(ctrl)||queue_down_prefix(p,(const uint8_t*)ctrl,(size_t)cl)!=0){
                log4c_error("id=%u queue fec ctrl start fail", p->id); return -1; }
        }
        log4c_info("middle id=%u fec v1.5 start sid=%u ctrl_sid=%llu route=%s loss=%.2f jitter=%.1fms",
            p->id, p->fec_session_id, (unsigned long long)p->down_stream_id, route, best_loss, best_jitter_ms);
        return 0;
    }
    p->down_opened=1;
    {
        char hdr[340]; int hl=snprintf(hdr,sizeof(hdr),"%d;%s\n",prio,route);
        if(hl<=0||(size_t)hl>=sizeof(hdr)||queue_down_prefix(p,(const uint8_t*)hdr,(size_t)hl)!=0){
            log4c_error("id=%u queue route to downstream fail", p->id); return -1; }
    }
    if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=stream-open age=%.1fms request_to_open=%.1fms sid=%llu route=%s",
        role_name(G.role),p->id,trace_delta_ms(p->downstream_open_at,p->created_at),
        trace_delta_ms(p->downstream_open_at,p->socks_request_at),(unsigned long long)p->down_stream_id,p->route);
    return 0;
}

/* 去程转发: left -> right */
static void fwd_down(proxy_stream_t* p, uint8_t* d, size_t n, int fin){
    ps_touch(p);
    ps_note_payload(p, 1, n);
    if(G.role==ROLE_EXIT){
        if(q2t_append(p,d,n)!=0){ p->need_teardown=1; return; }
        if(fin) p->q2t_fin=1;
    } else {
        if(G.role==ROLE_MIDDLE && p->fec_sidecar_mode && p->fec_ctrl_peer_ready){
            if(n||fin){
                if(nb_fec_tx_feed(p->fec_engine,d,n,fin,picoquic_current_time())!=NB_FEC_OK){ p->need_teardown=1; return; }
            }
            return;
        } else if(G.role==ROLE_MIDDLE && p->fec_sidecar_mode){
            if(fec_stage_bytes(p, d, n, fin) != 0) return;
            return;
        }
        if(n||fin){
            if(queue_down(p,d,n,fin)!=0) return;
        }
    }
}
/* 回程转发: right -> left */
static void fwd_up(proxy_stream_t* p, uint8_t* d, size_t n, int fin){
    ps_touch(p);
    ps_note_payload(p, 0, n);
    if(G.role==ROLE_ENTRY){
        if(q2t_append(p,d,n)!=0){ p->need_teardown=1; return; }
        if(fin) p->q2t_fin=1;
    } else { /* middle / exit: 写回上游 QUIC stream */
        if(G.role==ROLE_EXIT && p->fec_sidecar_mode && p->fec_ctrl_peer_ready){
            if(n||fin){
                if(nb_fec_tx_feed(p->fec_engine,d,n,fin,picoquic_current_time())!=NB_FEC_OK){ p->need_teardown=1; return; }
            }
            return;
        } else if(G.role==ROLE_EXIT && p->fec_sidecar_mode){
            if(fec_stage_bytes(p, d, n, fin) != 0) return;
            return;
        }
        if(n||fin){
            if(queue_up(p,d,n,fin)!=0) return;
        }
    }
}

static int udp_route_consume_h(const char* route,char* next_host,size_t host_cap,int* next_port,
    char* rest,size_t rest_cap){
    if(route==NULL||strncmp(route,"H:",2)!=0)return -1;
    const char* comma=strchr(route,',');
    if(comma==NULL||comma[1]==0)return -1;
    size_t first_len=(size_t)(comma-route);
    if(first_len<4||first_len>=NB_UDP_ROUTE_MAX||strlen(comma+1)>=rest_cap)return -1;
    char first[NB_UDP_ROUTE_MAX];memcpy(first,route,first_len);first[first_len]=0;
    char* colon=strrchr(first+2,':');if(colon==NULL)return -1;*colon=0;
    if(first[2]==0||strlen(first+2)>=host_cap||parse_port_strict(colon+1,next_port)!=0)return -1;
    snprintf(next_host,host_cap,"%s",first+2);snprintf(rest,rest_cap,"%s",comma+1);return 0;
}

static proxy_stream_t* udp_middle_open(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view,
    const char* route,char* rest,size_t rest_cap){
    char next_host[256];int next_port=0;
    if(udp_route_consume_h(route,next_host,sizeof(next_host),&next_port,rest,rest_cap)!=0)return NULL;
    struct sockaddr_storage addr;int is_name=0;
    if(picoquic_get_server_address(next_host,next_port,&addr,&is_name)!=0)return NULL;
    proxy_stream_t* p=ps_alloc();if(p==NULL)return NULL;p->udp_mode=1;p->udp_session_id=view->session_id;
    p->up_cnx=cnx;p->down_pool_id=pool_get_or_create(&addr);
    char target[256];int target_port=0;nb_flow_policy_t pol;
    if(parse_target_from_route(rest,target,sizeof(target),&target_port)==0){
        flow_policy_for_host(target,target_port,&pol);apply_flow_policy(p,&pol);
        snprintf(p->udp_target_host,sizeof(p->udp_target_host),"%s",target);p->udp_target_port=target_port;
    }else nb_flow_policy_default(&pol);
    if(p->down_pool_id<0){ps_set_close_reason(p,"udp-middle-pool-fail");ps_free(p);return NULL;}
    cnx_pool_t* pool=&G.pools[p->down_pool_id];int idx=pool_pick(pool,pol.lane_hint==NB_FLOW_LANE_LATENCY);
    if(idx<0){ps_set_close_reason(p,"udp-middle-no-path");ps_free(p);return NULL;}
    p->down_cnx=pool->cnx[idx];p->down_pool_idx=idx;snprintf(p->route,sizeof(p->route),"%s",rest);
    log4c_info("middle udp flow open id=%u usid=%u target=%s:%d route=%s",
        p->id,p->udp_session_id,p->udp_target_host,p->udp_target_port,p->route);return p;
}

static proxy_stream_t* udp_exit_open(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view,const char* route){
    char host[256];int port=0;if(parse_target_from_route(route,host,sizeof(host),&port)!=0)return NULL;
    if(!whitelist_allowed(host,port))return NULL;
    proxy_stream_t* p=ps_alloc();if(p==NULL)return NULL;
    p->udp_mode=1;p->udp_session_id=view->session_id;p->up_cnx=cnx;
    snprintf(p->route,sizeof(p->route),"%s",route);snprintf(p->udp_target_host,sizeof(p->udp_target_host),"%s",host);
    p->udp_target_port=port;p->target_port=port;nb_flow_policy_t pol;flow_policy_for_host(host,port,&pol);apply_flow_policy(p,&pol);
    if(dns_submit(p->id,host,port)!=0){ps_set_close_reason(p,"udp-dns-queue-full");ps_free(p);return NULL;}
    p->dns_pending=1;log4c_info("exit udp flow open id=%u usid=%u target=%s:%d",p->id,p->udp_session_id,host,port);return p;
}

static int udp_deliver_reassembled(proxy_stream_t* p,const nb_udp_reassembled_t* complete){
    if(G.role==ROLE_EXIT){
        if(p->udp_fd>=0){
            ssize_t n=send(p->udp_fd,complete->payload,complete->payload_length,MSG_NOSIGNAL);
            if(n!=(ssize_t)complete->payload_length)return -1;
            if(p->udp_first_target_send_at==0){
                p->udp_first_target_send_at=picoquic_current_time();
                log4c_info("exit udp trace id=%u usid=%u stage=target-first-send age=%.1fms ready_to_send=%.1fms bytes=%zu target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_target_send_at,p->created_at),
                    trace_delta_ms(p->udp_first_target_send_at,p->udp_target_ready_at),complete->payload_length,
                    p->udp_target_host,p->udp_target_port);
            }
        }else if(dgramq_append(&p->udp_pending_tx,&p->udp_pending_tx_len,&p->udp_pending_tx_cap,
            complete->payload,complete->payload_length,p->id)!=0)return -1;
        p->udp_packets_c2s++;p->bytes_c2s+=complete->payload_length;ps_touch(p);return 0;
    }
    if(G.role==ROLE_ENTRY){
        proxy_stream_t* parent=ps_find_by_id(p->udp_parent_id);
        if(parent==NULL||!parent->udp_association||!parent->udp_client_peer_set)return -1;
        char host[256];int port=0;if(parse_target_from_route(complete->route,host,sizeof(host),&port)!=0)return -1;
        uint8_t socks[NB_UDP_MAX_PAYLOAD+300];int n=nb_socks_udp_encode(socks,sizeof(socks),host,port,
            complete->payload,complete->payload_length);if(n<0)return -1;
        if(sendto(parent->udp_fd,socks,(size_t)n,MSG_NOSIGNAL,(struct sockaddr*)&parent->udp_client_peer,
            parent->udp_client_peer_len)!=n)return -1;
        if(p->udp_first_local_s2c_at==0){
            p->udp_first_local_s2c_at=picoquic_current_time();
            log4c_info("entry udp trace id=%u usid=%u stage=phone-first-send age=%.1fms rx_to_send=%.1fms bytes=%zu target=%s:%d",
                p->id,p->udp_session_id,trace_delta_ms(p->udp_first_local_s2c_at,p->created_at),
                trace_delta_ms(p->udp_first_local_s2c_at,p->udp_first_s2c_rx_at),complete->payload_length,
                p->udp_target_host,p->udp_target_port);
        }
        p->udp_packets_s2c++;p->bytes_s2c+=complete->payload_length;ps_touch(p);ps_touch(parent);return 0;
    }
    return -1;
}

static int udp_on_quic_datagram(picoquic_cnx_t* cnx,const nb_udp_wire_view_t* view){
    char route[NB_UDP_ROUTE_MAX];if(view->route_length>=sizeof(route))return -1;
    memcpy(route,view->route,view->route_length);route[view->route_length]=0;
    if(view->type==NB_UDP_TYPE_C2S){
        if(G.role==ROLE_MIDDLE){
            proxy_stream_t* p=ps_find_udp_up(cnx,view->session_id);char rest[NB_UDP_ROUTE_MAX];
            if(p==NULL)p=udp_middle_open(cnx,view,route,rest,sizeof(rest));
            else snprintf(rest,sizeof(rest),"%s",p->route);
            if(p!=NULL&&p->udp_first_c2s_rx_at==0){p->udp_first_c2s_rx_at=picoquic_current_time();
                log4c_info("middle udp trace id=%u usid=%u stage=c2s-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            if(p==NULL||udp_forward_fragment(p,1,view,rest)!=0)return -1;
            p->udp_packets_c2s+=(view->fragment_index==0);p->bytes_c2s+=view->payload_length;ps_touch(p);return 0;
        }
        if(G.role==ROLE_EXIT){
            proxy_stream_t* p=ps_find_udp_up(cnx,view->session_id);if(p==NULL)p=udp_exit_open(cnx,view,route);
            if(p==NULL)return -1;
            if(p->udp_first_c2s_rx_at==0){p->udp_first_c2s_rx_at=picoquic_current_time();
                log4c_info("exit udp trace id=%u usid=%u stage=c2s-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_c2s_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            nb_udp_reassembled_t complete;
            int rc=nb_udp_reassembly_feed(&p->udp_reassembly,view,picoquic_current_time(),&complete);
            return rc<0?-1:(rc==1?udp_deliver_reassembled(p,&complete):0);
        }
    }else if(view->type==NB_UDP_TYPE_S2C){
        if(G.role==ROLE_MIDDLE){
            proxy_stream_t* p=ps_find_udp_down(cnx,view->session_id);
            if(p!=NULL&&p->udp_first_s2c_rx_at==0){p->udp_first_s2c_rx_at=picoquic_current_time();
                log4c_info("middle udp trace id=%u usid=%u stage=s2c-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_s2c_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            if(p==NULL||udp_forward_fragment(p,0,view,p->route)!=0)return -1;
            p->udp_packets_s2c+=(view->fragment_index==0);p->bytes_s2c+=view->payload_length;ps_touch(p);return 0;
        }
        if(G.role==ROLE_ENTRY){
            proxy_stream_t* p=ps_find_udp_down(cnx,view->session_id);if(p==NULL)return -1;
            if(p->udp_first_s2c_rx_at==0){p->udp_first_s2c_rx_at=picoquic_current_time();
                log4c_info("entry udp trace id=%u usid=%u stage=s2c-first-rx age=%.1fms seq=%u frag=%u/%u bytes=%u target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_s2c_rx_at,p->created_at),view->sequence,
                    view->fragment_index+1,view->fragment_count,view->payload_length,p->udp_target_host,p->udp_target_port);}
            nb_udp_reassembled_t complete;int rc=nb_udp_reassembly_feed(&p->udp_reassembly,view,picoquic_current_time(),&complete);
            return rc<0?-1:(rc==1?udp_deliver_reassembled(p,&complete):0);
        }
    }
    return -1;
}

/* middle/exit: 收上一跳 stream 数据(去程)。首解析 route 首部, 再转发。 */
static int on_up_data(proxy_stream_t* p, uint8_t* bytes, size_t len, int fin){
    size_t off=0;
    if(p->fec_ctrl_only){
        if(len > 0) fec_ctrl_feed_bytes(p, bytes, len, 0);
        if(fin) p->up_fin_seen=1;
        return 0;
    }
    if(!p->hdr_done){
        while(off<len && p->hdr_len<sizeof(p->hdr)-1){
            char ch=(char)bytes[off++]; if(ch=='\n'){ p->hdr[p->hdr_len]=0; p->hdr_done=1; break; }
            p->hdr[p->hdr_len++]=ch;
        }
        if(!p->hdr_done && p->hdr_len >= sizeof(p->hdr)-1){
            char preview[512];
            p->hdr[p->hdr_len] = 0;
            fmt_route_preview(p->hdr, preview, sizeof(preview));
            log4c_error("id=%u route header overflow len=%zu preview=[%s]", p->id, p->hdr_len, preview);
            ps_teardown(p);
            return -1;
        }
        if(!p->hdr_done) return 0; /* 继续等首部 */
        if(p->hdr_len>=sizeof(p->route)){
            log4c_error("id=%u route exceeds internal limit len=%zu",p->id,p->hdr_len);
            ps_teardown(p);return -1;
        }
        memcpy(p->route,p->hdr,p->hdr_len+1);
        if(strncmp(p->hdr, NB_FEC_CTRL_PREFIX, strlen(NB_FEC_CTRL_PREFIX)) == 0){
            p->fec_ctrl_only=1;
            fec_ctrl_handle_line(p, p->hdr, 0);
            if(off < len) fec_ctrl_feed_bytes(p, bytes + off, len - off, 0);
            if(fin) p->up_fin_seen=1;
            return 0;
        }
        /* 新格式 <prio>;<route>；兼容读取旧格式 <prio>;0;<route>，但不再配对双发流。 */
        int prio=NB_PRIO_BULK; char* rt=p->hdr;
        { char* semi=strchr(rt,';');
          if(semi && semi>rt){ int ok=1; for(char* q=rt;q<semi;q++) if(*q<'0'||*q>'9'){ok=0;break;}
            if(ok){ prio=atoi(rt); rt=semi+1;
                char* semi2=strchr(rt,';');
                if(semi2&&semi2>rt){int old_numeric=1;for(char* q=rt;q<semi2;q++)if(*q<'0'||*q>'9'){old_numeric=0;break;}
                    if(old_numeric)rt=semi2+1;}
            } } }
        if(prio<=0||prio>255){log4c_error("id=%u invalid priority",p->id);ps_teardown(p);return -1;}
        p->prio=prio;
        picoquic_set_stream_priority(p->up_cnx,p->up_stream_id,(uint8_t)prio);
        /* 首部(去 prio 前缀后)第一段(到第一个 ',') = 本节点动作; 其余 = 转发给下一跳 */
        char first[300], rest[300]; rest[0]=0;
        char* comma=strchr(rt,',');
        if(comma){
            size_t fl=(size_t)(comma-rt),rl=strlen(comma+1);
            if(fl==0||fl>=sizeof(first)||rl==0||rl>=sizeof(rest)){
                log4c_error("id=%u route segment exceeds limit",p->id);ps_teardown(p);return -1;
            }
            memcpy(first,rt,fl); first[fl]=0; memcpy(rest,comma+1,rl+1);
        } else {
            size_t fl=strlen(rt);
            if(fl==0||fl>=sizeof(first)){log4c_error("id=%u route segment exceeds limit",p->id);ps_teardown(p);return -1;}
            memcpy(first,rt,fl+1);
        }

        if(strncmp(first,"T:",2)==0){
            /* exit: 异步解析 target 域名(不阻塞事件循环), 解析完成后在主循环发起非阻塞 connect */
            char thost[256]={0}; int tport=0; char* c=strrchr(first+2,':');
            if(c){size_t host_len=(size_t)(c-(first+2));
                if(host_len>0&&host_len<sizeof(thost)&&parse_port_strict(c+1,&tport)==0){memcpy(thost,first+2,host_len);thost[host_len]=0;}}
            if(c==NULL||thost[0]==0||tport<=0||tport>65535){log4c_error("id=%u malformed target route",p->id);ps_teardown(p);return -1;}
            { nb_flow_policy_t target_pol; flow_policy_for_host(thost,tport,&target_pol); apply_flow_policy(p,&target_pol); }
            if(!whitelist_allowed(thost,tport)){ /* 白名单外 -> 拒绝(reset stream), 出口访问控制 */
                log4c_debug("exit id=%u BLOCKED %s:%d (not in whitelist)",p->id,thost,tport);
                ps_teardown(p); return -1; }
            if(dns_submit(p->id,thost,tport)!=0){ log4c_error("id=%u dns queue full %s:%d",p->id,thost,tport);
                ps_teardown(p); return -1; }
            p->dns_pending=1; p->target_port=tport;
            log4c_debug("exit id=%u sid=%llu -> resolving %s:%d (async) prio=%d",p->id,(unsigned long long)p->up_stream_id,thost,tport,prio);
        } else if(strncmp(first,"H:",2)==0){
            /* middle: 动态连下一跳 QUIC(地址来自首部), 转发剩余 route(带同一 prio)。 */
            char nhost[256]={0}; int nport=0; char* c=strrchr(first+2,':');
            if(c){size_t host_len=(size_t)(c-(first+2));
                if(host_len>0&&host_len<sizeof(nhost)&&parse_port_strict(c+1,&nport)==0){memcpy(nhost,first+2,host_len);nhost[host_len]=0;}}
            if(c==NULL||nhost[0]==0||nport<=0||nport>65535||rest[0]==0){log4c_error("id=%u malformed hop route",p->id);ps_teardown(p);return -1;}
            struct sockaddr_storage naddr; int is_name=0;
            nb_flow_policy_t pol;
            char thost[256]; int tport=0;
            if(parse_target_from_route(rest[0]?rest:"", thost, sizeof(thost), &tport) == 0) flow_policy_for_host(thost, tport, &pol);
            else nb_flow_policy_default(&pol);
            if(!pol.matched) pol.prio = prio; /* 未命中 TikTok 规则时，回退使用上游携带的优先级 */
            if(picoquic_get_server_address(nhost,nport,&naddr,&is_name)!=0){
                log4c_error("id=%u resolve nexthop %s:%d fail",p->id,nhost,nport);
                ps_teardown(p); return -1; }
            p->down_pool_id=pool_get_or_create(&naddr);
            if(p->down_pool_id<0){ps_teardown(p);return -1;}
            if(downstream_open_stream(p, rest[0]?rest:"", &pol)!=0){ ps_teardown(p); return -1; }
            log4c_debug("middle id=%u up_sid=%llu -> nexthop %s:%d down_sid=%llu rest=[%s]",
                p->id,(unsigned long long)p->up_stream_id,nhost,nport,(unsigned long long)p->down_stream_id,rest);
        } else {
            char preview[512];
            fmt_route_preview(first, preview, sizeof(preview));
            log4c_error("id=%u bad route first-seg=[%s] hdr_len=%zu",p->id,preview,p->hdr_len);
            ps_teardown(p); return -1;
        }
    }
    if(off<len)fwd_down(p,bytes+off,len-off,0);
    if(fin){ p->up_fin_seen=1;
        fwd_down(p,NULL,0,1);
    }
    return 0;
}

/* QUIC 回调: 统一处理 up(去程) / down(回程) 两侧 stream 事件 */
static int relay_quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t ev, void* cb_ctx, void* v_stream_ctx){
    (void)cb_ctx;
    proxy_stream_t* p;
    switch(ev){
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin: {
        int fin=(ev==picoquic_callback_stream_fin);
        /* 已绑定 stream 直接用 picoquic app ctx；仅首包或旧 peer 回退线性查找。 */
        p=(proxy_stream_t*)v_stream_ctx;
        if(p==NULL||!p->in_use||!((p->down_cnx==cnx&&p->down_stream_id==stream_id)||
            (p->fec_ctrl_cnx==cnx&&p->fec_ctrl_stream_id==stream_id)))p=ps_find_by_down(cnx,stream_id);
        if(p!=NULL){
            if(p->fec_ctrl_cnx==cnx && p->fec_ctrl_stream_id==stream_id){
                if(length>0) fec_ctrl_feed_bytes(p,bytes,length,1);
                break;
            }
            if(length>0) fwd_up(p,bytes,length,0);
            if(fin){ p->down_fin_seen=1; if(G.role==ROLE_ENTRY) p->q2t_fin=1; else fwd_up(p,NULL,0,1); }
            break;
        }
        if(G.role==ROLE_MIDDLE&&pool_contains_cnx(cnx)){
            log4c_warn("middle stale downstream stream sid=%llu ignored",(unsigned long long)stream_id);
            picoquic_discard_stream(cnx,stream_id,0);
            break;
        }
        /* 再按上游(去程)匹配 */
        p=(proxy_stream_t*)v_stream_ctx;
        if(p==NULL||!p->in_use||!(p->up_cnx==cnx&&p->up_stream_id==stream_id))p=ps_find_by_up(cnx,stream_id);
        if(p==NULL){
            /* middle/exit: 新上游 stream(server 端被动收) */
            if(G.role==ROLE_ENTRY){ break; } /* entry 无 server, 忽略 */
            p=ps_alloc(); if(!p){ log4c_warn("stream table full, reset sid=%llu",(unsigned long long)stream_id);
                picoquic_discard_stream(cnx,stream_id,0); break; }
            p->up_cnx=cnx; p->up_stream_id=stream_id;
            picoquic_set_app_stream_ctx(cnx,stream_id,p);
        }
        on_up_data(p,bytes,length,fin);
        break; }
    case picoquic_callback_prepare_to_send: {
        p=(proxy_stream_t*)v_stream_ctx;
        if(p==NULL || !p->in_use) break;
        if(p->down_cnx==cnx && p->down_stream_id==stream_id){
            size_t prefix_left=p->down_prefix_len-p->down_prefix_off;
            if(prefix_left>0){
                size_t nb=prefix_left<length?prefix_left:length;
                int still_active=(prefix_left>nb)||p->down_tx.len>0||p->down_tx_fin;
                uint8_t* dst=picoquic_provide_stream_data_buffer(bytes,nb,0,still_active);
                if(dst!=NULL&&nb>0){
                    if(p->down_prefix_prepare_at==0){
                        p->down_prefix_prepare_at=picoquic_current_time();
                        if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=route-prepare age=%.1fms queue_wait=%.1fms bytes=%zu allowance=%zu route=%s",
                            role_name(G.role),p->id,trace_delta_ms(p->down_prefix_prepare_at,p->created_at),
                            trace_delta_ms(p->down_prefix_prepare_at,p->down_prefix_queued_at),nb,length,p->route);
                    }
                    memcpy(dst,p->down_prefix+p->down_prefix_off,nb);
                    p->down_prefix_off+=nb;
                    if(p->down_prefix_off==p->down_prefix_len){
                        p->down_prefix_off=0;
                        p->down_prefix_len=0;
                        /* route 已送入 QUIC，后续 payload 恢复策略优先级，避免
                         * 新流首部长期抢占既有实时媒体数据。 */
                        picoquic_set_stream_priority(cnx,stream_id,(uint8_t)p->prio);
                    }
                    ps_touch(p);
                }else if(dst==NULL&&nb>0){
                    log4c_warn("id=%u provide down prefix fail sid=%llu",p->id,(unsigned long long)stream_id);
                }
                break;
            }
            size_t nb = (p->down_tx.len < length) ? p->down_tx.len : length;
            int is_fin = (p->down_tx_fin && nb == p->down_tx.len);
            int still_active = (p->down_tx.len > nb) || (p->down_tx_fin && !is_fin);
            uint8_t* dst = picoquic_provide_stream_data_buffer(bytes, nb, is_fin, still_active);
            if(dst != NULL && nb > 0){
                if(p->first_down_prepare_at==0){
                    p->first_down_prepare_at=picoquic_current_time();
                    if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=c2s-prepare age=%.1fms queue_wait=%.1fms bytes=%zu q_before=%zu allowance=%zu route=%s",
                        role_name(G.role),p->id,trace_delta_ms(p->first_down_prepare_at,p->created_at),
                        trace_delta_ms(p->first_down_prepare_at,p->first_down_queue_at),nb,p->down_tx.len,length,p->route);
                }
                (void)nb_ring_copyout(&p->down_tx,dst,nb);
                ps_touch(p);
            } else if(dst == NULL && (nb > 0 || is_fin)) {
                log4c_warn("id=%u provide down buffer fail sid=%llu", p->id, (unsigned long long)stream_id);
            }
            if(is_fin) p->down_tx_fin = 0;
        } else if(p->up_cnx==cnx && p->up_stream_id==stream_id){
            size_t nb = (p->up_tx.len < length) ? p->up_tx.len : length;
            int is_fin = (p->up_tx_fin && nb == p->up_tx.len);
            int still_active = (p->up_tx.len > nb) || (p->up_tx_fin && !is_fin);
            uint8_t* dst = picoquic_provide_stream_data_buffer(bytes, nb, is_fin, still_active);
            if(dst != NULL && nb > 0){
                if(p->first_up_prepare_at==0){
                    p->first_up_prepare_at=picoquic_current_time();
                    if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=s2c-prepare age=%.1fms queue_wait=%.1fms bytes=%zu q_before=%zu allowance=%zu route=%s",
                        role_name(G.role),p->id,trace_delta_ms(p->first_up_prepare_at,p->created_at),
                        trace_delta_ms(p->first_up_prepare_at,p->first_up_queue_at),nb,p->up_tx.len,length,p->route);
                }
                (void)nb_ring_copyout(&p->up_tx,dst,nb);
                ps_touch(p);
            } else if(dst == NULL && (nb > 0 || is_fin)) {
                log4c_warn("id=%u provide up buffer fail sid=%llu", p->id, (unsigned long long)stream_id);
            }
            if(is_fin) p->up_tx_fin = 0;
        } else {
            (void)picoquic_provide_stream_data_buffer(bytes, 0, 0, 0);
        }
        break; }
    case picoquic_callback_prepare_datagram: {
        int handled = 0;
        for(int i=0;i<MAX_CONN&&!handled;i++){
            p=&G.streams[i];if(!p->in_use||!p->udp_mode||p->udp_association)continue;
            uint8_t* queue=NULL;size_t* queue_len=NULL;
            if(p->down_cnx==cnx&&p->udp_down_tx_len>=2){queue=p->udp_down_tx;queue_len=&p->udp_down_tx_len;}
            else if(p->up_cnx==cnx&&p->udp_up_tx_len>=2){queue=p->udp_up_tx;queue_len=&p->udp_up_tx_len;}
            if(queue==NULL)continue;
            const uint8_t* dg=NULL;size_t dg_len=0;
            if(!dgramq_peek(queue,*queue_len,&dg,&dg_len))continue;
            if(dg_len>length){
                (void)picoquic_provide_datagram_buffer_ex(bytes,0,picoquic_datagram_active_any_path);
                handled=1;break;
            }
            uint8_t* dst=picoquic_provide_datagram_buffer_ex(bytes,dg_len,picoquic_datagram_active_any_path);
            if(dst!=NULL){
                uint64_t now=picoquic_current_time();
                if(queue==p->udp_down_tx&&p->udp_first_c2s_prepare_at==0){
                    p->udp_first_c2s_prepare_at=now;
                    uint64_t queued_at=p->udp_first_local_c2s_at?p->udp_first_local_c2s_at:p->udp_first_c2s_rx_at;
                    log4c_info("%s udp trace id=%u usid=%u stage=c2s-first-prepare age=%.1fms queue_wait=%.1fms wire_bytes=%zu allowance=%zu target=%s:%d",
                        role_name(G.role),p->id,p->udp_session_id,trace_delta_ms(now,p->created_at),
                        trace_delta_ms(now,queued_at),dg_len,length,p->udp_target_host,p->udp_target_port);
                }else if(queue==p->udp_up_tx&&p->udp_first_s2c_prepare_at==0){
                    p->udp_first_s2c_prepare_at=now;
                    uint64_t queued_at=p->udp_first_target_rx_at?p->udp_first_target_rx_at:p->udp_first_s2c_rx_at;
                    log4c_info("%s udp trace id=%u usid=%u stage=s2c-first-prepare age=%.1fms queue_wait=%.1fms wire_bytes=%zu allowance=%zu target=%s:%d",
                        role_name(G.role),p->id,p->udp_session_id,trace_delta_ms(now,p->created_at),
                        trace_delta_ms(now,queued_at),dg_len,length,p->udp_target_host,p->udp_target_port);
                }
                memcpy(dst,dg,dg_len);dgramq_consume(queue,queue_len);handled=1;
            }
        }
        for(int i=0;i<MAX_CONN&&!handled;i++){
            p = &G.streams[i];
            if(!p->in_use || p->fec_dg_cnx != cnx || p->fec_dg_tx_len < 2) continue;
            const uint8_t* dg = NULL; size_t dg_len = 0;
            if(!dgramq_peek(p->fec_dg_tx, p->fec_dg_tx_len, &dg, &dg_len)) continue;
            if(dg_len > length){
                (void)picoquic_provide_datagram_buffer_ex(bytes, 0, picoquic_datagram_active_any_path);
                handled = 1;
                break;
            }
            uint8_t* dst = picoquic_provide_datagram_buffer_ex(bytes, dg_len,
                (p->fec_dg_tx_len > (2 + dg_len)) ? picoquic_datagram_active_any_path : picoquic_datagram_not_active);
            if(dst != NULL){
                memcpy(dst, dg, dg_len);
                dgramq_consume(p->fec_dg_tx, &p->fec_dg_tx_len);
                handled = 1;
                break;
            }
        }
        if(!handled){
            (void)picoquic_provide_datagram_buffer_ex(bytes, 0, picoquic_datagram_not_active);
        }
        break; }
    case picoquic_callback_datagram:
    case picoquic_callback_datagram_acked:
    case picoquic_callback_datagram_lost:
    case picoquic_callback_datagram_spurious: {
        if(length>=24&&nb_get_u32(bytes)==NB_UDP_MAGIC){
            if(ev==picoquic_callback_datagram){
                nb_udp_wire_view_t view;
                if(nb_udp_wire_decode(bytes,length,&view)!=0||udp_on_quic_datagram(cnx,&view)!=0){
                    log4c_warn("%s udp datagram reject length=%zu",role_name(G.role),length);
                }
            }
            break;
        }
        if(length>=20 && nb_get_u32(bytes)==NB_FEC_DGRAM_MAGIC && bytes[4]==NB_FEC_PROTOCOL_VERSION &&
            (bytes[5]==nb_fec_dgram_source || bytes[5]==nb_fec_dgram_repair)){
            uint32_t sid=nb_get_u32(bytes+6);
            p=ps_find_by_fec_session(sid);
            if(p!=NULL&&p->fec_engine!=NULL){
                if(ev==picoquic_callback_datagram){
                    uint32_t block_id=nb_get_u32(bytes+10); uint8_t symbol_idx=bytes[14];
                    p->fec_dg_recv++;
                    if(!fec_should_drop(bytes[5],block_id,symbol_idx)){
                        int rc=nb_fec_on_datagram(p->fec_engine,bytes,length,picoquic_current_time());
                        if(rc!=NB_FEC_OK){ log4c_warn("%s fec engine datagram fail sid=%u rc=%d",role_name(G.role),sid,rc); p->need_teardown=1; }
                    }
                }else if(ev==picoquic_callback_datagram_acked){ p->fec_dg_acked++; }
                else if(ev==picoquic_callback_datagram_lost){ p->fec_dg_lost++; }
            }
            break;
        }
        if(length >= 14 && nb_get_u32(bytes) == NB_FEC_DGRAM_MAGIC && bytes[4] == NB_FEC_DGRAM_VER){
            uint8_t dgt = bytes[5];
            uint32_t sid = nb_get_u32(bytes + 6);
            uint32_t seq = (length >= 14) ? nb_get_u32(bytes + 10) : 0;
            p = ps_find_by_fec_session(sid);
            if(p != NULL){
                if(ev == picoquic_callback_datagram){
                    p->fec_dg_recv++;
                    log4c_debug("%s fec dgram recv sid=%u type=%u seq=%u", role_name(G.role), sid, dgt, seq);
                    if(G.role == ROLE_EXIT && dgt == nb_fec_dgram_hello){
                        p->fec_dg_cnx = cnx;
                        p->fec_ctrl_peer_ready = 1;
                        if((p->fec_stage_tx_len > 0 || p->fec_stage_tx_fin) && !p->need_teardown){
                            if(nb_fec_tx_feed(p->fec_engine,p->fec_stage_tx,p->fec_stage_tx_len,
                                p->fec_stage_tx_fin,picoquic_current_time())!=NB_FEC_OK){
                                p->need_teardown = 1;
                            }
                            p->fec_stage_tx_len = 0;
                            p->fec_stage_tx_fin = 0;
                        }
                        (void)fec_queue_probe_datagram(p, nb_fec_dgram_ack);
                    } else if(G.role == ROLE_MIDDLE && dgt == nb_fec_dgram_ack){
                        p->fec_ctrl_peer_ready = 1;
                        if((p->fec_stage_tx_len > 0 || p->fec_stage_tx_fin) && !p->need_teardown){
                            if(nb_fec_tx_feed(p->fec_engine,p->fec_stage_tx,p->fec_stage_tx_len,
                                p->fec_stage_tx_fin,picoquic_current_time())!=NB_FEC_OK){
                                p->need_teardown = 1;
                            }
                            p->fec_stage_tx_len = 0;
                            p->fec_stage_tx_fin = 0;
                        }
                    }
                } else if(ev == picoquic_callback_datagram_acked){
                    p->fec_dg_acked++;
                } else if(ev == picoquic_callback_datagram_lost){
                    p->fec_dg_lost++;
                }
            }
        }
        break; }
    case picoquic_callback_stream_reset:
    case picoquic_callback_stop_sending:
        /* 对端 reset/stop 单条 stream -> 主动 teardown, discard 另一侧 stream 连锁通知(middle 据此回收) */
        if((p=ps_find_by_down(cnx,stream_id))!=NULL){ ps_teardown_reason(p,ev==picoquic_callback_stream_reset?"quic-stream-reset":"quic-stop-sending"); }
        else if((p=ps_find_by_up(cnx,stream_id))!=NULL){ ps_teardown_reason(p,ev==picoquic_callback_stream_reset?"quic-stream-reset":"quic-stop-sending"); }
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset: {
        const char* close_reason=ev==picoquic_callback_close?"quic-connection-close":
            (ev==picoquic_callback_application_close?"quic-application-close":"quic-stateless-reset");
        int i; for(i=0;i<MAX_CONN;i++) if(G.streams[i].in_use && (G.streams[i].up_cnx==cnx||G.streams[i].down_cnx==cnx||G.streams[i].fec_ctrl_cnx==cnx)){
            ps_set_close_reason(&G.streams[i],close_reason);ps_free(&G.streams[i]);
        }
        for(int pool_id=0;pool_id<G.pool_count;pool_id++)for(i=0;i<POOL_SIZE;i++)if(G.pools[pool_id].cnx[i]==cnx){
            cnx_pool_t* pool=&G.pools[pool_id];
            pool->cnx[i]=NULL;pool->recent_loss[i]=0;pool->recent_rtt[i]=0;pool->recent_rtt_max[i]=0;
            pool->recent_sent[i]=0;pool->last_sent_total[i]=0;pool->last_lost_total[i]=0;
            pool->last_timer_total[i]=0;pool->last_spurious_total[i]=0;
            pool->last_retrans_total[i]=0;pool->last_preempt_total[i]=0;
            pool->recent_rtt_var[i]=0;pool->recent_ts[i]=0;
        }
        picoquic_set_callback(cnx,NULL,NULL); break; }
    default: break;
    }
    return 0;
}

/* entry: 建到第一跳的 QUIC 连接池(POOL_SIZE 条并行连接) */
static int entry_open_upstream(void){
    return pool_get_or_create(&G.next_addr)<0?-1:0;
}

static int socks_udp_bind_socket(struct sockaddr_in* bound){
    uint32_t attempts=(g_socks_udp_port_min&&g_socks_udp_port_max)
        ?(uint32_t)g_socks_udp_port_max-g_socks_udp_port_min+1u:1u;
    for(uint32_t i=0;i<attempts;i++){
        int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return -1;
        struct sockaddr_in addr;memset(&addr,0,sizeof(addr));addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=INADDR_ANY;
        if(g_socks_udp_port_min){
            uint32_t offset=((uint32_t)g_socks_udp_port_next-g_socks_udp_port_min+i)%attempts;
            addr.sin_port=htons((uint16_t)(g_socks_udp_port_min+offset));
        }
        if(set_nonblock(fd)==0&&bind(fd,(struct sockaddr*)&addr,sizeof(addr))==0){
            socklen_t addr_len=sizeof(addr);getsockname(fd,(struct sockaddr*)&addr,&addr_len);
            if(g_socks_udp_port_min){uint16_t port=ntohs(addr.sin_port);
                g_socks_udp_port_next=(port==g_socks_udp_port_max)?g_socks_udp_port_min:(uint16_t)(port+1);}
            *bound=addr;return fd;
        }
        int saved=errno;close(fd);errno=saved;
        if(!g_socks_udp_port_min||errno!=EADDRINUSE)return -1;
    }
    errno=EADDRINUSE;return -1;
}

/* entry: 接受一个 TCP 客户端连接 */
static void entry_accept(void){
    struct sockaddr_storage peer;
    socklen_t peer_len=sizeof(peer);
    int fd=accept(G.tcp_listen_fd,(struct sockaddr*)&peer,&peer_len);
    if(fd<0) return;
    set_nonblock(fd); int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    proxy_stream_t* p=ps_alloc();
    if(!p){ log4c_warn("stream table full, drop tcp accept"); close(fd); return; }
    p->tcp_fd=fd;
    sockaddr_to_text((struct sockaddr*)&peer,peer_len,p->peer_addr,sizeof(p->peer_addr));
    p->udp_tcp_peer=peer;p->udp_tcp_peer_len=peer_len;
    p->down_pool_id=0;
    if(epoll_tcp_update(p,1)!=0){log4c_warn("entry id=%u epoll add fail: %s",p->id,strerror(errno));ps_free(p);return;}
    if(G.socks_enabled){
        p->socks_stage=0; /* 等 SOCKS5 握手拿到 target 后再开 QUIC stream */
        log4c_debug("entry id=%u accept fd=%d (socks, awaiting handshake)",p->id,fd);
        return;
    }
    snprintf(p->route,sizeof(p->route),"%s",G.route_str);
    { nb_flow_policy_t pol; char thost[256]="-"; int tport=0;
      int parsed = (parse_target_from_route(G.route_str, thost, sizeof(thost), &tport) == 0);
      if(parsed) flow_policy_for_host(thost, tport, &pol);
      else nb_flow_policy_default(&pol);
      int prio = pol.prio;
      log4c_info("entry id=%u classify fixed route=%s host=%s port=%d class=%s lane=%s fec=%s rule=%s prio=%d",
        p->id, G.route_str, parsed ? thost : "-", parsed ? tport : 0,
        nb_flow_class_name(pol.flow_class), nb_flow_lane_name(pol.lane_hint), nb_flow_fec_name(pol.fec_hint), pol.rule_name, prio);
      if(downstream_open_stream(p,G.route_str,&pol)!=0){ ps_free(p); return; } }
    log4c_info("entry id=%u accept fd=%d -> down_sid=%llu route=%s",p->id,fd,(unsigned long long)p->down_stream_id,G.route_str);
}

/* entry SOCKS5 入口: 推进握手, 完成后用动态 target 开 QUIC stream。 */
static void socks_handshake(proxy_stream_t* p){
    uint8_t buf[512];
    ssize_t n=recv(p->tcp_fd,buf,sizeof(buf),0);
    if(n<=0){ /* EOF 或错误(非 EAGAIN) -> 握手阶段客户端断开, 回收 */
        if(n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)) ps_teardown(p);
        return; }
    if(p->socks_len+(size_t)n>sizeof(p->socks_buf)){ log4c_warn("id=%u socks buf overflow",p->id); ps_free(p); return; }
    memcpy(p->socks_buf+p->socks_len,buf,(size_t)n); p->socks_len+=(size_t)n;

    if(p->socks_stage==0){ /* greeting: VER NMETHODS METHODS... */
        if(p->socks_len<2) return;
        if(p->socks_buf[0]!=0x05){ log4c_warn("id=%u not socks5 (ver=%u)",p->id,p->socks_buf[0]); ps_free(p); return; }
        uint8_t nm=p->socks_buf[1];
        if(p->socks_len<(size_t)(2+nm)) return; /* 等齐 */
        uint8_t wanted=g_socks_auth_enabled?0x02:0x00,method=0xff;
        for(uint8_t i=0;i<nm;i++)if(p->socks_buf[2+i]==wanted){method=wanted;break;}
        char methods[128];
        socks_methods_to_text(p->socks_buf + 2, nm, methods, sizeof(methods));
        uint8_t rep[2]={0x05,method};
        (void)send(p->tcp_fd,rep,2,MSG_NOSIGNAL);
        size_t used=2+nm; memmove(p->socks_buf,p->socks_buf+used,p->socks_len-used); p->socks_len-=used;
        if(method==0xff){
            log4c_warn("entry id=%u socks reject peer=%s offered=[%s] required=0x%02x reason=no-acceptable-auth-method",
                p->id,p->peer_addr,methods,wanted);
            ps_set_close_reason(p,"socks-auth-method");ps_free(p);return;
        }
        p->socks_stage=g_socks_auth_enabled?1:2;
        p->socks_greeting_at=picoquic_current_time();
    }
    if(p->socks_stage==1){ /* RFC 1929: VER ULEN UNAME PLEN PASSWD */
        if(p->socks_len<2)return;
        if(p->socks_buf[0]!=0x01){ps_free(p);return;}
        size_t ulen=p->socks_buf[1];
        if(ulen==0||ulen>=NB_AUTH_NAME_MAX){uint8_t r[2]={1,1};(void)send(p->tcp_fd,r,2,MSG_NOSIGNAL);ps_free(p);return;}
        if(p->socks_len<2+ulen+1)return;
        size_t plen=p->socks_buf[2+ulen],used=3+ulen+plen;
        if(plen==0||p->socks_len<used)return;
        char username[NB_AUTH_NAME_MAX];memcpy(username,p->socks_buf+2,ulen);username[ulen]=0;
        int cache_hit=0;
        int valid=nb_auth_user_verify_cached(&g_socks_users,username,p->socks_buf+3+ulen,plen,
            picoquic_current_time(),SOCKS_AUTH_CACHE_TTL_US,&cache_hit);
        uint8_t r[2]={1,valid?0:1};(void)send(p->tcp_fd,r,2,MSG_NOSIGNAL);
        memset(p->socks_buf+3+ulen,0,plen);
        memmove(p->socks_buf,p->socks_buf+used,p->socks_len-used);p->socks_len-=used;
        if(!valid){log4c_warn("entry id=%u socks authentication failed peer=%s user=%s",p->id,p->peer_addr,username);ps_free(p);return;}
        log4c_debug("entry id=%u socks authenticated user=%s cache=%s",p->id,username,cache_hit?"hit":"miss");
        p->socks_auth_at=picoquic_current_time();
        p->socks_stage=2;
    }
    if(p->socks_stage==2){ /* request: VER CMD RSV ATYP ADDR PORT */
        if(p->socks_len<4) return;
        uint8_t cmd=p->socks_buf[1], atyp=p->socks_buf[3];
        char host[256]={0}; int port=0; size_t need=0;
        if(atyp==0x01){ need=4+4+2; if(p->socks_len<need) return;
            snprintf(host,sizeof(host),"%u.%u.%u.%u",p->socks_buf[4],p->socks_buf[5],p->socks_buf[6],p->socks_buf[7]);
            port=(p->socks_buf[8]<<8)|p->socks_buf[9]; }
        else if(atyp==0x03){ uint8_t dl=p->socks_buf[4]; need=4+1+(size_t)dl+2; if(p->socks_len<need) return;
            memcpy(host,p->socks_buf+5,dl); host[dl]=0; port=(p->socks_buf[5+dl]<<8)|p->socks_buf[6+dl]; }
        else if(atyp==0x04){ need=4+16+2; if(p->socks_len<need) return;
            inet_ntop(AF_INET6,p->socks_buf+4,host,sizeof(host)); port=(p->socks_buf[20]<<8)|p->socks_buf[21]; }
        else {
            log4c_warn("entry id=%u socks request peer=%s cmd=%s(0x%02x) atyp=%s(0x%02x) result=reject reason=unsupported-atyp",
                p->id,p->peer_addr,socks_cmd_name(cmd),cmd,socks_atyp_name(atyp),atyp);
            uint8_t r[10]={0x05,0x08,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
            ps_set_close_reason(p,"socks-unsupported-atyp");ps_free(p);return;
        }
        p->socks_request_at=picoquic_current_time();
        log4c_info("entry id=%u socks request peer=%s cmd=%s(0x%02x) atyp=%s(0x%02x) target=%s:%d accept_age=%.1fms socks_ms=greeting:%.1f,auth:%.1f,request:%.1f",
            p->id,p->peer_addr,socks_cmd_name(cmd),cmd,socks_atyp_name(atyp),atyp,host,port,
            trace_delta_ms(p->socks_request_at,p->created_at),
            trace_delta_ms(p->socks_greeting_at,p->created_at),
            trace_delta_ms(p->socks_auth_at,p->socks_greeting_at),
            trace_delta_ms(p->socks_request_at,p->socks_auth_at?p->socks_auth_at:p->socks_greeting_at));
        if(cmd==0x03){
            struct sockaddr_in bind_addr;int ufd=socks_udp_bind_socket(&bind_addr);
            if(ufd<0){
                int saved=errno;if(ufd>=0)close(ufd);
                log4c_warn("entry id=%u UDP ASSOCIATE bind fail peer=%s range=%u-%u error=%s",
                    p->id,p->peer_addr,g_socks_udp_port_min,g_socks_udp_port_max,strerror(saved));
                uint8_t r[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};(void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
                ps_set_close_reason(p,"udp-associate-bind-fail");ps_free(p);return;
            }
            p->udp_mode=1;p->udp_association=1;p->udp_fd=ufd;p->socks_stage=3;
            if(epoll_udp_update(p,1)!=0){
                uint8_t r[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};(void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
                ps_set_close_reason(p,"udp-associate-epoll-fail");ps_free(p);return;
            }
            uint16_t relay_port=ntohs(bind_addr.sin_port);struct in_addr relay_addr={0};
            if(g_socks_udp_advertise_configured)relay_addr=g_socks_udp_advertise_addr;
            else {struct sockaddr_in local;socklen_t local_len=sizeof(local);memset(&local,0,sizeof(local));
                if(getsockname(p->tcp_fd,(struct sockaddr*)&local,&local_len)==0&&local.sin_family==AF_INET)
                    relay_addr=local.sin_addr;}
            if(relay_addr.s_addr==INADDR_ANY){
                log4c_warn("entry id=%u UDP ASSOCIATE has no reachable advertise address",p->id);
                uint8_t fail[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};(void)send(p->tcp_fd,fail,10,MSG_NOSIGNAL);
                ps_set_close_reason(p,"udp-associate-no-address");ps_free(p);return;
            }
            uint8_t r[10]={0x05,0x00,0,0x01,0,0,0,0,(uint8_t)(relay_port>>8),(uint8_t)relay_port};
            memcpy(r+4,&relay_addr,sizeof(relay_addr));
            (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
            char relay_ip[INET_ADDRSTRLEN];inet_ntop(AF_INET,&relay_addr,relay_ip,sizeof(relay_ip));
            log4c_info("entry id=%u UDP ASSOCIATE ready peer=%s relay=%s:%u accept_age=%.1fms request_to_ready=%.1fms",
                p->id,p->peer_addr,relay_ip,relay_port,trace_delta_ms(picoquic_current_time(),p->created_at),
                trace_delta_ms(picoquic_current_time(),p->socks_request_at));
            memmove(p->socks_buf,p->socks_buf+need,p->socks_len-need);p->socks_len-=need;
            return;
        }
        if(cmd!=0x01){ /* 仅支持 CONNECT */
            log4c_warn("entry id=%u socks request peer=%s cmd=%s(0x%02x) atyp=%s(0x%02x) target=%s:%d result=reject reason=unsupported-command",
                p->id,p->peer_addr,socks_cmd_name(cmd),cmd,socks_atyp_name(atyp),atyp,host,port);
            uint8_t r[10]={0x05,0x07,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);
            ps_set_close_reason(p,"socks-unsupported-command");ps_free(p);return; }
        if(!whitelist_allowed(host,port)){ /* 白名单外 -> 拒绝(0x02 not allowed by ruleset), 不占三跳线路 */
            log4c_debug("entry id=%u BLOCKED %s:%d (not in whitelist)",p->id,host,port);
            uint8_t r[10]={0x05,0x02,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL); ps_free(p); return; }
        uint8_t rep[10]={0x05,0x00,0x00,0x01,0,0,0,0,0,0}; /* 成功, BND 全 0 */
        (void)send(p->tcp_fd,rep,10,MSG_NOSIGNAL);
        memmove(p->socks_buf,p->socks_buf+need,p->socks_len-need); p->socks_len-=need;
        /* 构造 route: 中间跳前缀(可空) + 动态 target */
        char route[300];
        const nb_route_entry_t* selected=G.exit_routes_enabled?nb_routes_pick(&G.exit_routes):NULL;
        const char* mid=selected?selected->hop:G.mid_route;
        int route_len=mid[0]
            ? snprintf(route,sizeof(route),"%s,T:%s:%d",mid,host,port)
            : snprintf(route,sizeof(route),"T:%s:%d",host,port);
        if(route_len<0||(size_t)route_len>=sizeof(route)){
            log4c_warn("entry id=%u route too long host=%s",p->id,host);
            uint8_t r[10]={0x05,0x01,0,0x01,0,0,0,0,0,0};
            (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL);ps_free(p);return;
        }
        snprintf(p->route,sizeof(p->route),"%s",route);
        nb_flow_policy_t pol; flow_policy_for_host(host, port, &pol);
        int prio=pol.prio;
        log4c_info("entry id=%u classify peer=%s host=%s port=%d class=%s lane=%s fec=%s rule=%s prio=%d route=%s",
            p->id, p->peer_addr, host, port, nb_flow_class_name(pol.flow_class), nb_flow_lane_name(pol.lane_hint),
            nb_flow_fec_name(pol.fec_hint), pol.rule_name, prio, route);
        if(selected)log4c_debug("entry id=%u selected exit route=%s hop=%s",p->id,selected->name,selected->hop);
        if(downstream_open_stream(p,route,&pol)!=0){ ps_free(p); return; }
        p->socks_stage=3;
        log4c_debug("entry id=%u socks CONNECT %s:%d -> down_sid=%llu route=%s",p->id,host,port,(unsigned long long)p->down_stream_id,route);
        /* request 之后可能已跟随应用数据 -> 转发到下游 */
        if(p->socks_len>0){ fwd_down(p,p->socks_buf,p->socks_len,0); p->socks_len=0; }
    }
}

/* 把面向 TCP 的待写缓冲(q2t) flush 到 TCP fd */
static void flush_q2t(proxy_stream_t* p){
    if(p->tcp_fd<0) return;
    while(p->q2t.len>0){
        const uint8_t* data=NULL;size_t avail=nb_ring_peek(&p->q2t,&data);
        ssize_t n=send(p->tcp_fd,data,avail,MSG_NOSIGNAL);
        if(n>0){
            if(p->first_q2t_flush_at==0){
                p->first_q2t_flush_at=picoquic_current_time();
                if(ps_is_rtc_webcast(p)) log4c_info("%s tcp trace id=%u stage=%s age=%.1fms queue_wait=%.1fms bytes=%zd q_before=%zu route=%s",
                    role_name(G.role),p->id,G.role==ROLE_EXIT?"target-write":"client-write",
                    trace_delta_ms(p->first_q2t_flush_at,p->created_at),
                    trace_delta_ms(p->first_q2t_flush_at,p->first_q2t_queue_at),n,p->q2t.len,p->route);
            }
            nb_ring_consume(&p->q2t,(size_t)n); ps_touch(p);
        }
        else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK){
            /* 本地 TCP 已死(EPIPE/ECONNRESET/...): 待写数据无处可去, 丢弃并标记 EOF -> 触发回收,
             * 否则 q2t 恒非空使 maybe_free 永不满足, 流泄漏。 */
            log4c_debug("id=%u q2t send err=%s, drop %zu & teardown",p->id,strerror(errno),p->q2t.len);
            ps_set_close_reason(p,"tcp-send-error");
            p->tcp_eof=1; nb_ring_clear(&p->q2t);
            break;
        }
        else break; /* EAGAIN: 等下轮可写 */
    }
    if(p->q2t.len==0 && p->q2t_fin){ shutdown(p->tcp_fd,SHUT_WR); p->q2t_fin=0; }
}

static int tcp_read_allowed(proxy_stream_t* p){
    size_t main_q=(G.role==ROLE_ENTRY)?p->down_tx.len:p->up_tx.len;
    size_t stage_q=p->fec_stage_tx_len;
    size_t dg_q=p->fec_dg_tx_len;
    if(p->tcp_read_paused){
        if(main_q<=NB_TCP_TX_LOW&&stage_q<=NB_TCP_TX_LOW&&dg_q<=NB_FEC_DG_LOW)p->tcp_read_paused=0;
    }else if(main_q>=NB_TCP_TX_HIGH||stage_q>=NB_TCP_TX_HIGH||dg_q>=NB_FEC_DG_HIGH){
        p->tcp_read_paused=1;
        log4c_debug("id=%u tcp read paused main=%zu stage=%zu dg=%zu",p->id,main_q,stage_q,dg_q);
    }
    return !p->tcp_read_paused;
}

static int epoll_tcp_update(proxy_stream_t* p,int add){
    if(G.epoll_fd<0||p==NULL||!p->in_use||p->tcp_fd<0)return -1;
    uint32_t events=EPOLLERR|EPOLLHUP|EPOLLRDHUP;
    if((G.socks_enabled&&p->socks_stage<3)||p->tcp_connecting||tcp_read_allowed(p))events|=EPOLLIN;
    if(p->q2t.len>0||p->tcp_connecting)events|=EPOLLOUT;
    size_t index=(size_t)(p-G.streams);if(index>=MAX_CONN)return -1;
    struct epoll_event ev;memset(&ev,0,sizeof(ev));ev.events=events;
    ev.data.u64=NB_EPOLL_TAG_STREAM|((uint64_t)p->id<<16)|(uint64_t)index;
    int op=add?EPOLL_CTL_ADD:EPOLL_CTL_MOD;
    if(epoll_ctl(G.epoll_fd,op,p->tcp_fd,&ev)==0)return 0;
    if(!add&&errno==ENOENT)return epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,p->tcp_fd,&ev);
    return -1;
}

static int epoll_udp_update(proxy_stream_t* p,int add){
    if(G.epoll_fd<0||p==NULL||!p->in_use||p->udp_fd<0)return -1;
    size_t index=(size_t)(p-G.streams);if(index>=MAX_CONN)return -1;
    struct epoll_event ev;memset(&ev,0,sizeof(ev));ev.events=EPOLLIN|EPOLLERR;
    ev.data.u64=NB_EPOLL_TAG_UDP_SESSION|((uint64_t)p->id<<16)|(uint64_t)index;
    return epoll_ctl(G.epoll_fd,add?EPOLL_CTL_ADD:EPOLL_CTL_MOD,p->udp_fd,&ev);
}

static proxy_stream_t* udp_entry_child_get(proxy_stream_t* parent,const char* host,int port){
    proxy_stream_t* p=ps_find_udp_child(parent->id,host,port);if(p!=NULL)return p;
    if(!whitelist_allowed(host,port))return NULL;
    p=ps_alloc();if(p==NULL)return NULL;p->udp_mode=1;p->udp_parent_id=parent->id;
    p->udp_session_id=udp_session_id_new();p->down_pool_id=0;
    if(p->udp_session_id==0||G.pool_count<=0){ps_set_close_reason(p,"udp-child-init-fail");ps_free(p);return NULL;}
    snprintf(p->udp_target_host,sizeof(p->udp_target_host),"%s",host);p->udp_target_port=port;
    const nb_route_entry_t* selected=G.exit_routes_enabled?nb_routes_pick(&G.exit_routes):NULL;
    const char* mid=selected?selected->hop:G.mid_route;
    int n=mid[0]?snprintf(p->route,sizeof(p->route),"%s,T:%s:%d",mid,host,port):
        snprintf(p->route,sizeof(p->route),"T:%s:%d",host,port);
    if(n<=0||(size_t)n>=sizeof(p->route)){ps_set_close_reason(p,"udp-route-overflow");ps_free(p);return NULL;}
    nb_flow_policy_t pol;flow_policy_for_host(host,port,&pol);apply_flow_policy(p,&pol);
    cnx_pool_t* pool=&G.pools[0];int idx=pool_pick(pool,pol.lane_hint==NB_FLOW_LANE_LATENCY);
    if(idx<0){ps_set_close_reason(p,"udp-no-downstream");ps_free(p);return NULL;}
    p->down_cnx=pool->cnx[idx];p->down_pool_idx=idx;
    log4c_info("entry udp flow open id=%u parent=%u usid=%u peer=%s target=%s:%d class=%s route=%s",
        p->id,parent->id,p->udp_session_id,parent->peer_addr,host,port,nb_flow_class_name(pol.flow_class),p->route);
    return p;
}

static void udp_local_drain(proxy_stream_t* p){
    uint8_t buffer[65535+300];
    for(;;){
        if(p->udp_association){
            struct sockaddr_storage from;socklen_t from_len=sizeof(from);
            ssize_t n=recvfrom(p->udp_fd,buffer,sizeof(buffer),0,(struct sockaddr*)&from,&from_len);
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
            if(n<=0){if(n<0)log4c_warn("entry UDP relay recv fail id=%u error=%s",p->id,strerror(errno));break;}
            if(!sockaddr_ip_equal(&from,&p->udp_tcp_peer)){
                char text[96];sockaddr_to_text((struct sockaddr*)&from,from_len,text,sizeof(text));
                log4c_warn("entry UDP relay reject id=%u source=%s tcp_peer=%s",p->id,text,p->peer_addr);continue;
            }
            if(!p->udp_client_peer_set){p->udp_client_peer=from;p->udp_client_peer_len=from_len;p->udp_client_peer_set=1;}
            else if(!sockaddr_ip_equal(&from,&p->udp_client_peer))continue;
            char host[256];int port=0;const uint8_t* payload=NULL;size_t payload_len=0;
            if(nb_socks_udp_parse(buffer,(size_t)n,host,sizeof(host),&port,&payload,&payload_len)!=0){
                log4c_warn("entry UDP relay malformed id=%u bytes=%zd",p->id,n);continue;
            }
            proxy_stream_t* child=udp_entry_child_get(p,host,port);if(child==NULL)continue;
            if(child->udp_first_local_c2s_at==0){
                child->udp_first_local_c2s_at=picoquic_current_time();
                log4c_info("entry udp trace id=%u usid=%u stage=phone-first-rx association_age=%.1fms flow_age=%.1fms bytes=%zu target=%s:%d",
                    child->id,child->udp_session_id,trace_delta_ms(child->udp_first_local_c2s_at,p->created_at),
                    trace_delta_ms(child->udp_first_local_c2s_at,child->created_at),payload_len,host,port);
            }
            if(udp_queue_packet(child,1,NB_UDP_TYPE_C2S,child->route,payload,payload_len)!=0){
                ps_teardown_reason(child,"udp-down-queue-fail");continue;
            }
            child->udp_packets_c2s++;child->bytes_c2s+=payload_len;ps_touch(child);ps_touch(p);
        }else if(G.role==ROLE_EXIT){
            ssize_t n=recv(p->udp_fd,buffer,NB_UDP_MAX_PAYLOAD,0);
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
            if(n<=0){if(n<0)log4c_warn("exit UDP recv fail id=%u error=%s",p->id,strerror(errno));break;}
            if(p->udp_first_target_rx_at==0){
                p->udp_first_target_rx_at=picoquic_current_time();
                log4c_info("exit udp trace id=%u usid=%u stage=target-first-rx age=%.1fms send_to_rx=%.1fms bytes=%zd target=%s:%d",
                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_target_rx_at,p->created_at),
                    trace_delta_ms(p->udp_first_target_rx_at,p->udp_first_target_send_at),n,
                    p->udp_target_host,p->udp_target_port);
            }
            if(udp_queue_packet(p,0,NB_UDP_TYPE_S2C,p->route,buffer,(size_t)n)!=0){
                ps_teardown_reason(p,"udp-up-queue-fail");break;
            }
            p->udp_packets_s2c++;p->bytes_s2c+=(size_t)n;ps_touch(p);
        }else break;
    }
}

/* 从 TCP fd 读 -> add_to_stream 到 QUIC。entry: 写下游; exit: 写上游(回程)。 */
static void pump_tcp(proxy_stream_t* p){
    uint8_t buf[16384]; ssize_t n;
    for(;;){
        n=recv(p->tcp_fd,buf,sizeof(buf),0);
        if(n>0){
            if(G.role==ROLE_ENTRY) fwd_down(p,buf,(size_t)n,0); else fwd_up(p,buf,(size_t)n,0);
            if(p->need_teardown||!tcp_read_allowed(p))break;
        }
        else if(n==0){ p->tcp_eof=1;ps_set_close_reason(p,G.role==ROLE_ENTRY?"client-eof":"target-eof");
            if(G.role==ROLE_ENTRY) fwd_down(p,NULL,0,1); else fwd_up(p,NULL,0,1);
            break; }
        else if(errno==EAGAIN||errno==EWOULDBLOCK)break;
        else{
            log4c_debug("id=%u tcp recv err=%s",p->id,strerror(errno));p->tcp_eof=1;
            ps_set_close_reason(p,"tcp-recv-error");
            if(G.role==ROLE_ENTRY)fwd_down(p,NULL,0,1);else fwd_up(p,NULL,0,1);break;
        }
    }
}

/* 判定并释放已完成的流。
 * 关键: 真实服务器(HTTP keep-alive)响应完不主动关 TCP, 若死等"双向 FIN 齐全"则流永不回收 -> 泄漏。
 * 但反向的坑更隐蔽: exit 侧回程响应是 add_to_stream 排队在上游 QUIC stream 里的(不占 q2t),
 * target 一关(tcp_eof)队列往往还没发完; 若此时 discard 会截断回程(大文件只传一部分)。故:
 *   1) 双向数据方向都结束 且 q2t 排空 -> 优雅回收;
 *   2) 仅 ENTRY: client TCP 关闭即事务结束, 回程无处可送, 主动 teardown(丢回程 OK, 且连锁 reset 清下游);
 *   3) EXIT 不主动 teardown: 必须让上游 stream 把排队响应发完(靠 client 收完关闭触发的去程 FIN
 *      或 entry 的连锁 reset 或空闲超时来收尾), 否则截断大流量回程。 */
static void maybe_free(proxy_stream_t* p){
    int left_done  = (G.role==ROLE_ENTRY)? p->tcp_eof : p->up_fin_seen;
    int right_done = (G.role==ROLE_EXIT) ? p->tcp_eof : p->down_fin_seen;
    if(left_done && right_done && p->q2t.len==0){ ps_teardown_reason(p,"bidirectional-fin"); return; }
    if(G.role==ROLE_ENTRY && p->tcp_eof && p->q2t.len==0){ ps_teardown_reason(p,"entry-client-eof"); return; }
}

/* P1-2a: recvmmsg 批量收 UDP -> 降低高包率(大流量)下的接收 syscall 开销。
 * 单线程事件循环, 用 function-local static 缓冲复用, 一次 syscall 收多达 UDP_RECV_BATCH 个包。 */
#define UDP_RECV_BATCH 32
static void udp_drain(uint64_t now){
    static struct mmsghdr msgs[UDP_RECV_BATCH];
    static struct iovec iovs[UDP_RECV_BATCH];
    static uint8_t bufs[UDP_RECV_BATCH][1536];
    static struct sockaddr_storage froms[UDP_RECV_BATCH];
    static int inited=0;
    if(!inited){
        for(int i=0;i<UDP_RECV_BATCH;i++){
            iovs[i].iov_base=bufs[i]; iovs[i].iov_len=sizeof(bufs[i]);
            msgs[i].msg_hdr.msg_iov=&iovs[i]; msgs[i].msg_hdr.msg_iovlen=1;
            msgs[i].msg_hdr.msg_name=&froms[i]; msgs[i].msg_hdr.msg_control=NULL; msgs[i].msg_hdr.msg_controllen=0;
        }
        inited=1;
    }
    for(;;){
        for(int i=0;i<UDP_RECV_BATCH;i++) msgs[i].msg_hdr.msg_namelen=sizeof(froms[i]); /* namelen 是 in/out, 每次重置 */
        int got=recvmmsg(G.udp_fd,msgs,UDP_RECV_BATCH,0,NULL);
        if(got<=0) break;
        for(int m=0;m<got;m++)
            picoquic_incoming_packet(G.quic,bufs[m],(size_t)msgs[m].msg_len,
                (struct sockaddr*)&froms[m],(struct sockaddr*)&G.local_addr,0,0,now);
        if(got<UDP_RECV_BATCH) break; /* 已排空 */
    }
}

/* P1-2b: UDP GSO 批量发送。picoquic 逐包 prepare, 我们把连续的、同目的地址、同段大小的包攒进一个
 * 缓冲, 用 sendmsg + UDP_SEGMENT 一次 syscall 发出(内核分段)。段大小需统一(除末段可更小)。
 * GSO 不支持(老内核/EIO)时逐段 sendto 回退, 保证正确性。 */
static int addr_eq(const struct sockaddr_storage* a, const struct sockaddr_storage* b){
    if(a->ss_family!=b->ss_family) return 0;
    if(a->ss_family==AF_INET){ const struct sockaddr_in*x=(const void*)a,*y=(const void*)b;
        return x->sin_port==y->sin_port && x->sin_addr.s_addr==y->sin_addr.s_addr; }
    const struct sockaddr_in6*x=(const void*)a,*y=(const void*)b;
    return x->sin6_port==y->sin6_port && memcmp(&x->sin6_addr,&y->sin6_addr,16)==0;
}
static void udp_send_gso(uint8_t* buf, size_t len, int segsz, struct sockaddr* to, socklen_t tolen){
    if(len==0||segsz<=0) return;
    if(len<=(size_t)segsz){ (void)sendto(G.udp_fd,buf,len,0,to,tolen); return; } /* 单段无需 GSO */
    struct msghdr mh; memset(&mh,0,sizeof(mh));
    struct iovec iov={buf,len}; mh.msg_name=to; mh.msg_namelen=tolen; mh.msg_iov=&iov; mh.msg_iovlen=1;
    char cbuf[CMSG_SPACE(sizeof(uint16_t))]; memset(cbuf,0,sizeof(cbuf));
    mh.msg_control=cbuf; mh.msg_controllen=sizeof(cbuf);
    struct cmsghdr* cm=CMSG_FIRSTHDR(&mh);
    cm->cmsg_level=SOL_UDP; cm->cmsg_type=UDP_SEGMENT; cm->cmsg_len=CMSG_LEN(sizeof(uint16_t));
    *((uint16_t*)CMSG_DATA(cm))=(uint16_t)segsz;
    if(sendmsg(G.udp_fd,&mh,0)>=0) return;
    for(size_t off=0;off<len;off+=segsz){ size_t s=(len-off<(size_t)segsz)?(len-off):(size_t)segsz;
        (void)sendto(G.udp_fd,buf+off,s,0,to,tolen); } /* 回退逐段 */
}
static void udp_send_batch(uint64_t now){
    uint8_t gbuf[64*1536]; size_t glen=0; int gseg=0; struct sockaddr_storage gto; socklen_t gtolen=0;
    for(;;){
        uint8_t sb[1536]; size_t sl=0; struct sockaddr_storage to,fr; int ifx=0;
        picoquic_connection_id_t lc; picoquic_cnx_t* lcnx=NULL;
        int r=picoquic_prepare_next_packet(G.quic,now,sb,sizeof(sb),&sl,&to,&fr,&ifx,&lc,&lcnx);
        if(r!=0||sl==0) break;
        socklen_t tl=(to.ss_family==AF_INET)?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
        if(!g_udp_gso_enabled){
            if(sendto(G.udp_fd,sb,sl,0,(struct sockaddr*)&to,tl)<0){
                static uint64_t last_warn=0;
                if(now-last_warn>=1000000ULL){
                    last_warn=now;
                    log4c_warn("udp sendto failed: %s",strerror(errno));
                }
            }
            continue;
        }
        if(glen==0){ memcpy(&gto,&to,tl); gtolen=tl; gseg=(int)sl; memcpy(gbuf,sb,sl); glen=sl; }
        else if(addr_eq(&to,&gto) && (int)sl<=gseg && glen+sl<=sizeof(gbuf)){
            memcpy(gbuf+glen,sb,sl); glen+=sl;
            if((int)sl<gseg){ udp_send_gso(gbuf,glen,gseg,(struct sockaddr*)&gto,gtolen); glen=0; gseg=0; } /* 末段更小 -> flush */
        } else { /* 目的/段大小变化或缓冲满 -> flush 后新起一批 */
            udp_send_gso(gbuf,glen,gseg,(struct sockaddr*)&gto,gtolen);
            memcpy(&gto,&to,tl); gtolen=tl; gseg=(int)sl; memcpy(gbuf,sb,sl); glen=sl;
        }
    }
    if(glen>0) udp_send_gso(gbuf,glen,gseg,(struct sockaddr*)&gto,gtolen);
}

static void usage(const char* prog){
    fprintf(stderr,
        "Usage:\n"
        "  entry(固定target): %s -r entry -l <tcp_port> -n <hop1_host> -N <hop1_qport> -R <route> -c <cert> -k <key> -a <ca>\n"
        "  entry(SOCKS5入口): %s -r entry -l <socks_port> -n <hop1_host> -N <hop1_qport> -S -U <users> [-M <mid_route>] -c <cert> -k <key> -a <ca>\n"
        "  middle           : %s -r middle -p <quic_port> -c <cert> -k <key> -a <ca>\n"
        "  exit             : %s -r exit   -p <quic_port> -c <cert> -k <key> -a <ca>\n"
        "  route(发给第一跳): \"H:hop2:qport,...,T:target:port\" (两跳仅 \"T:target:port\")\n"
        "  -S: SOCKS5 入口, target 由 SOCKS5 CONNECT 动态获取; -M: 中间跳前缀如 \"H:kz:4443\"(空=两跳)\n"
        "  -F: TikTok 内部分流规则文件(默认尝试 /root/nb/tiktok_flow_rules.conf 或环境变量 NB_TIKTOK_RULES)\n",
        prog,prog,prog,prog);
}

int main(int argc,char**argv){
    int i; int listen_port=0, quic_port=0, next_port=0;
    const char* rolestr=NULL; const char* next_host=NULL;
    const char* cert=NULL; const char* key=NULL; const char* cert_root=NULL; const char* route=NULL;
    const char* users_path=NULL;
    const char* control_path_arg=NULL;
    const char* exit_routes_path=NULL;
    const char* outbound_ip=NULL;
    char control_path[108];
    const char* wl_path=NULL;
    const char* tiktok_rules_path=NULL;
    memset(&G,0,sizeof(G)); G.tcp_listen_fd=-1; G.udp_fd=-1; G.epoll_fd=-1;G.control_fd=-1;

    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"-r")&&i+1<argc) rolestr=argv[++i];
        else if(!strcmp(argv[i],"-l")&&i+1<argc) listen_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-p")&&i+1<argc) quic_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-n")&&i+1<argc) next_host=argv[++i];
        else if(!strcmp(argv[i],"-N")&&i+1<argc) next_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-R")&&i+1<argc) route=argv[++i];
        else if(!strcmp(argv[i],"-S")) G.socks_enabled=1;
        else if(!strcmp(argv[i],"-W")&&i+1<argc) wl_path=argv[++i];
        else if(!strcmp(argv[i],"-F")&&i+1<argc) tiktok_rules_path=argv[++i];
        else if(!strcmp(argv[i],"-M")&&i+1<argc) snprintf(G.mid_route,sizeof(G.mid_route),"%s",argv[++i]);
        else if(!strcmp(argv[i],"-c")&&i+1<argc) cert=argv[++i];
        else if(!strcmp(argv[i],"-k")&&i+1<argc) key=argv[++i];
        else if(!strcmp(argv[i],"-a")&&i+1<argc) cert_root=argv[++i];
        else if(!strcmp(argv[i],"-U")&&i+1<argc) users_path=argv[++i];
        else if(!strcmp(argv[i],"-C")&&i+1<argc) control_path_arg=argv[++i];
        else if(!strcmp(argv[i],"-E")&&i+1<argc) exit_routes_path=argv[++i];
        else if(!strcmp(argv[i],"-o")&&i+1<argc) outbound_ip=argv[++i];
    }
    if(rolestr==NULL){ usage(argv[0]); return 1; }
    if(!strcmp(rolestr,"entry")) G.role=ROLE_ENTRY;
    else if(!strcmp(rolestr,"middle")) G.role=ROLE_MIDDLE;
    else if(!strcmp(rolestr,"exit")) G.role=ROLE_EXIT;
    else { usage(argv[0]); return 1; }
    { const char* worker=getenv("NB_WORKER_ID");if(!worker)worker="0";
      int n=control_path_arg?snprintf(control_path,sizeof(control_path),"%s",control_path_arg)
          :snprintf(control_path,sizeof(control_path),"/run/nb-%s-%s.ctl",role_name(G.role),worker);
      if(n<0||(size_t)n>=sizeof(control_path)){fprintf(stderr,"control socket path too long\n");return 1;} }

    { char pname[32]; snprintf(pname,sizeof(pname),"nb-%s",role_name(G.role)); log4c_init(pname);
      /* 生产默认 INFO(高频 per-stream 日志已降为 DEBUG, 不输出), 避免海量流日志拖慢单线程事件循环。
       * 需调试时 NB_LOG_LEVEL=DEBUG。 */
      const char* lv=getenv("NB_LOG_LEVEL"); int lvl=LOG4C_INFO;
      if(lv){ if(!strcmp(lv,"DEBUG"))lvl=LOG4C_DEBUG; else if(!strcmp(lv,"WARN"))lvl=LOG4C_WARN; else if(!strcmp(lv,"ERROR"))lvl=LOG4C_ERROR; }
      log4c_set_level(lvl); }
    nb_runtime_init();

    { const char* ug=getenv("NB_UDP_GSO");
      if(ug&&(!strcmp(ug,"off")||!strcmp(ug,"0")))g_udp_gso_enabled=0;
      log4c_info("UDP GSO aggregation: %s",g_udp_gso_enabled?"enabled":"disabled"); }
    { const char* rg=getenv("NB_REORDER_GAP"); const char* rd=getenv("NB_REORDER_DELAY_US");
      if(rg&&rg[0])g_reorder_gap=strtoull(rg,NULL,10);
      if(rd&&rd[0])g_reorder_delay_us=strtoull(rd,NULL,10);
      if(G.role==ROLE_ENTRY || G.role==ROLE_MIDDLE)log4c_info("%s loss reorder tolerance: gap=%llu delay=%lluus (PTO unchanged)",
          role_name(G.role),(unsigned long long)g_reorder_gap,(unsigned long long)g_reorder_delay_us); }

    { const char* fe=getenv("NB_FEC");
      if(fe&&fe[0])log4c_warn("NB_FEC is retired and ignored; use NB_FEC_V15_ACTIVE only for controlled tests"); }
    { const char* fv=getenv("NB_FEC_V15");
      if(fv && (!strcmp(fv,"off")||!strcmp(fv,"0"))){ g_fec_v15_observe=0; }
      if(fv && (!strcmp(fv,"on")||!strcmp(fv,"1"))){
          log4c_warn("NB_FEC_V15=%s now means observe-only; use NB_FEC_V15_ACTIVE=on only in controlled tests", fv);
      }
      const char* fa=getenv("NB_FEC_V15_ACTIVE");
      if(fa && (!strcmp(fa,"on")||!strcmp(fa,"1"))){
          g_fec_v15_active=1;
          g_fec_v15_observe=1;
      }
      log4c_info("FEC v1.5 mode: observe=%d active=%d", g_fec_v15_observe, g_fec_v15_active); }
    { const char* ff=getenv("NB_FEC_V15_FORCE"); /* V1.5 测试强开: 无需 line_bad 即开启 sidecar */
      if(ff && (!strcmp(ff,"on")||!strcmp(ff,"1"))){ g_fec_v15_force=1; log4c_info("FEC v1.5 sidecar force mode: ENABLED (NB_FEC_V15_FORCE=%s)",ff); } }
    { const char* ds=getenv("NB_FEC_V15_DROP_SRC_MOD");
      if(ds && atoi(ds)>0){ g_fec_v15_drop_src_mod=(uint32_t)atoi(ds); log4c_info("FEC v1.5 test drop source mod=%u", g_fec_v15_drop_src_mod); } }
    { const char* dr=getenv("NB_FEC_V15_DROP_REPAIR_MOD");
      if(dr && atoi(dr)>0){ g_fec_v15_drop_repair_mod=(uint32_t)atoi(dr); log4c_info("FEC v1.5 test drop repair mod=%u", g_fec_v15_drop_repair_mod); } }
    {
      uint64_t bitrate=20000000ULL, rtt_us=250000ULL; unsigned mult=2;
      const char* eb=getenv("NB_FEC_BDP_BITRATE_BPS"); if(eb&&strtoull(eb,NULL,10)>0) bitrate=strtoull(eb,NULL,10);
      const char* er=getenv("NB_FEC_BDP_RTT_US"); if(er&&strtoull(er,NULL,10)>0) rtt_us=strtoull(er,NULL,10);
      const char* em=getenv("NB_FEC_BDP_MULTIPLIER"); if(em&&atoi(em)>0) mult=(unsigned)atoi(em);
      if(nb_fec_config_for_bdp(&g_fec_cfg,NB_FEC_V15_K,NB_FEC_V15_R,bitrate,rtt_us,mult)!=NB_FEC_OK){
          log4c_error("FEC config invalid"); log4c_shutdown(); return 1;
      }
      log4c_info("FEC engine cfg k=%u r=%u history=%u blocks rx_window=%u blocks hold=%lluus nack=%lluus retry=%lluus/%u",
          g_fec_cfg.k,g_fec_cfg.r,g_fec_cfg.tx_history_blocks,g_fec_cfg.rx_window_blocks,
          (unsigned long long)g_fec_cfg.block_hold_us,(unsigned long long)g_fec_cfg.nack_delay_us,
          (unsigned long long)g_fec_cfg.nack_retry_us,g_fec_cfg.max_nack_retries);
    }

    if(wl_init(wl_path)!=0){
        log4c_error("whitelist requested but could not be loaded; refusing to start");
        log4c_shutdown();return 1;
    }
    nb_policy_init(tiktok_rules_path ? tiktok_rules_path : getenv("NB_TIKTOK_RULES"));

    int insecure_test=0;
    { const char* value=getenv("NB_INSECURE_TEST_MODE");insecure_test=value&&(!strcmp(value,"1")||!strcmp(value,"on")); }
    if(!insecure_test&&(!cert||!key||!cert_root)){
        log4c_error("secure mode requires -c node certificate, -k private key and -a CA certificate");
        log4c_shutdown();return 1;
    }
    if(!insecure_test&&((G.role==ROLE_ENTRY&&G.socks_enabled)||G.role==ROLE_EXIT)&&!wl_path){
        log4c_error("secure SOCKS entry and exit require fail-closed -W <whitelist>");
        log4c_shutdown();return 1;
    }
    if(insecure_test)log4c_warn("NB_INSECURE_TEST_MODE enabled: node certificate authentication is not enforced");
    if(key){
        char error[256];
        if(nb_auth_check_private_file(key,error,sizeof(error))!=0){
            log4c_error("private key rejected: %s",error);log4c_shutdown();return 1;
        }
    }
    if(G.role==ROLE_ENTRY&&G.socks_enabled){
        const char* udp_advertise=getenv("NB_SOCKS_UDP_ADVERTISE_IP");
        if(udp_advertise&&udp_advertise[0]){
            if(inet_pton(AF_INET,udp_advertise,&g_socks_udp_advertise_addr)!=1){
                log4c_error("invalid NB_SOCKS_UDP_ADVERTISE_IP=%s",udp_advertise);log4c_shutdown();return 1;
            }
            g_socks_udp_advertise_configured=1;
            log4c_info("SOCKS UDP advertise address: %s",udp_advertise);
        }else log4c_info("SOCKS UDP advertise address: accepted TCP local address");
        const char* udp_port_min=getenv("NB_SOCKS_UDP_PORT_MIN");
        const char* udp_port_max=getenv("NB_SOCKS_UDP_PORT_MAX");
        if((udp_port_min&&udp_port_min[0])||(udp_port_max&&udp_port_max[0])){
            char* end_min=NULL;char* end_max=NULL;unsigned long min_port=udp_port_min?strtoul(udp_port_min,&end_min,10):0;
            unsigned long max_port=udp_port_max?strtoul(udp_port_max,&end_max,10):0;
            if(!udp_port_min||!udp_port_max||end_min==udp_port_min||end_max==udp_port_max||*end_min||*end_max||
                min_port<1024||max_port>65535||min_port>max_port||max_port-min_port+1>16384){
                log4c_error("invalid SOCKS UDP port range min=%s max=%s",
                    udp_port_min?udp_port_min:"(unset)",udp_port_max?udp_port_max:"(unset)");log4c_shutdown();return 1;
            }
            g_socks_udp_port_min=(uint16_t)min_port;g_socks_udp_port_max=(uint16_t)max_port;
            g_socks_udp_port_next=g_socks_udp_port_min;
            log4c_info("SOCKS UDP relay port range: %u-%u capacity=%u",g_socks_udp_port_min,
                g_socks_udp_port_max,(unsigned)(g_socks_udp_port_max-g_socks_udp_port_min+1));
        }else log4c_info("SOCKS UDP relay port range: ephemeral");
        if(!users_path&&!insecure_test){log4c_error("secure SOCKS mode requires -U <users file>");log4c_shutdown();return 1;}
        if(users_path){
            char error[256];
            if(nb_auth_users_load(&g_socks_users,users_path,error,sizeof(error))!=0){
                log4c_error("SOCKS users rejected: %s",error);log4c_shutdown();return 1;
            }
            g_socks_auth_enabled=1;log4c_info("SOCKS authentication loaded users=%zu",g_socks_users.count);
        }
        if(exit_routes_path){
            char error[256];
            if(nb_routes_load(&G.exit_routes,exit_routes_path,error,sizeof(error))!=0){
                log4c_error("exit routes rejected: %s",error);log4c_shutdown();return 1;
            }
            G.exit_routes_enabled=1;log4c_info("exit routes loaded count=%zu",G.exit_routes.count);
        }
    }

    if(G.role==ROLE_ENTRY){
        int is_name=0;
        if(!listen_port||!next_host||!next_port||(!G.socks_enabled && !route)){ usage(argv[0]); log4c_shutdown(); return 1; }
        if(picoquic_get_server_address(next_host,next_port,&G.next_addr,&is_name)!=0){
            log4c_error("resolve hop1 %s:%d fail",next_host,next_port); log4c_shutdown(); return 1; }
        G.next_configured=1;
        if(!G.socks_enabled){
            size_t route_len=strlen(route);
            if(route_len==0||route_len>=sizeof(G.route_str)){log4c_error("fixed route exceeds limit");log4c_shutdown();return 1;}
            memcpy(G.route_str,route,route_len+1);
        }
    } else {
        if(!quic_port){ usage(argv[0]); log4c_shutdown(); return 1; }
    }
    if(outbound_ip){
        if(G.role!=ROLE_EXIT){log4c_error("-o outbound source address is only valid for exit");log4c_shutdown();return 1;}
        struct sockaddr_in* source=(struct sockaddr_in*)&G.outbound_addr;
        memset(&G.outbound_addr,0,sizeof(G.outbound_addr));source->sin_family=AF_INET;
        if(inet_pton(AF_INET,outbound_ip,&source->sin_addr)!=1){log4c_error("invalid IPv4 outbound source: %s",outbound_ip);log4c_shutdown();return 1;}
        source->sin_port=0;G.outbound_configured=1;log4c_info("exit outbound source: %s",outbound_ip);
    }

    G.quic=picoquic_create(MAX_CONN, cert, key, cert_root, NB_ALPN,
        G.role==ROLE_ENTRY?NULL:relay_quic_callback, G.role==ROLE_ENTRY?NULL:&G, NULL,NULL,NULL,
        picoquic_current_time(), NULL,NULL,NULL,0);
    if(G.quic==NULL){ log4c_error("quic create fail"); log4c_shutdown(); return 1; }
    if(G.role!=ROLE_ENTRY){
        picoquic_set_cookie_mode(G.quic,2);
        if(!insecure_test)picoquic_set_client_authentication(G.quic,1);
    }
    { const char* cc=getenv("NB_CC");
      const char* bbr_options=getenv("NB_BBR_OPTIONS");
      if(cc&&strcmp(cc,"cubic")==0){
          picoquic_set_default_congestion_algorithm(G.quic,picoquic_cubic_algorithm);
          log4c_info("congestion control: cubic");
      }else if(cc&&strcmp(cc,"dcubic")==0){
          picoquic_set_default_congestion_algorithm(G.quic,picoquic_dcubic_algorithm);
          log4c_info("congestion control: dcubic");
      }else if(cc&&strcmp(cc,"fastcc")==0){
          picoquic_set_default_congestion_algorithm(G.quic,picoquic_fastcc_algorithm);
          log4c_info("congestion control: fastcc");
      }else{
          if(cc&&cc[0]&&strcmp(cc,"bbr")!=0)
              log4c_warn("unknown NB_CC=%s, fallback to bbr",cc);
          if(bbr_options&&bbr_options[0])
              picoquic_set_default_congestion_algorithm_ex(G.quic,picoquic_bbr_algorithm,bbr_options);
          else
              picoquic_set_default_congestion_algorithm(G.quic,picoquic_bbr_algorithm);
          log4c_info("congestion control: bbr options=%s",(bbr_options&&bbr_options[0])?bbr_options:"default");
      } }
    { const char* value=getenv("NB_CWIN_MAX_BYTES");
      if(value&&value[0]){
          char* end=NULL; errno=0;
          unsigned long long limit=strtoull(value,&end,10);
          if(errno!=0||end==value||*end!='\0'||limit<65536ULL||limit>67108864ULL){
              log4c_error("invalid NB_CWIN_MAX_BYTES=%s (expected 65536..67108864)",value);
              log4c_shutdown(); return 1;
          }
          picoquic_set_cwin_max(G.quic,(uint64_t)limit);
          log4c_info("congestion window cap: %llu bytes (%lluKB)",limit,limit/1024ULL);
      }else{
          log4c_info("congestion window cap: unlimited");
      } }
    /* 放开传输参数上限。单流吞吐 ≈ max_stream_data / RTT: 三跳 gz→hk→kz RTT~300ms 下,
     * 旧 1MiB 窗口把单流限死在 ~26Mbps。放到 8MiB -> 单流理论上限 ~210Mbps(覆盖 80Mbps 峰值)。
     * connection 级 max_data 64MiB 支撑大量并发流叠加吞吐, 并缓解多流共享连接窗口的背压。 */
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_streams_bidi, 2000);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_stream_data_bidi_local, 8388608);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_stream_data_bidi_remote, 8388608);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_data, 67108864);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_max_datagram_frame_size, PICOQUIC_MAX_PACKET_SIZE);
    /* 优化2: 降低 max_ack_delay(默认25ms)到 5ms -> 接收方更快回 ACK, 拥塞控制更灵敏、RTT 感知更低,
     * 对直播/实时流延迟直接有益(代价: ACK 包略增, 80Mbps 下开销可忽略)。 */
    picoquic_set_default_tp_value(G.quic, picoquic_tp_max_ack_delay, 5000);

    /* UDP socket: entry 用临时端口(client); middle/exit bind quic_port(server)。 */
    G.udp_fd=socket(AF_INET,SOCK_DGRAM,0);
    { int one=1;if(setsockopt(G.udp_fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one))!=0){
        log4c_error("udp SO_REUSEPORT failed: %s",strerror(errno));log4c_shutdown();return 1;
    } }
    /* 调大 UDP 收发缓冲(默认~200KB, 高吞吐/突发下溢出丢包致 QUIC 重传崩)。
     * SO_*BUFFORCE(需 root/CAP_NET_ADMIN)绕过 net.core.rmem_max 上限, 失败回退普通 SO_*BUF。 */
    { int bufsz=16*1024*1024, rb=0, sb=0; socklen_t ol=sizeof(int);
      if(setsockopt(G.udp_fd,SOL_SOCKET,SO_RCVBUFFORCE,&bufsz,sizeof(bufsz))<0)
          setsockopt(G.udp_fd,SOL_SOCKET,SO_RCVBUF,&bufsz,sizeof(bufsz));
      if(setsockopt(G.udp_fd,SOL_SOCKET,SO_SNDBUFFORCE,&bufsz,sizeof(bufsz))<0)
          setsockopt(G.udp_fd,SOL_SOCKET,SO_SNDBUF,&bufsz,sizeof(bufsz));
      getsockopt(G.udp_fd,SOL_SOCKET,SO_RCVBUF,&rb,&ol);
      getsockopt(G.udp_fd,SOL_SOCKET,SO_SNDBUF,&sb,&ol);
      log4c_info("udp buffers rcvbuf=%d sndbuf=%d (req %d; 若<请求值则受 net.core.*mem_max 限)",rb,sb,bufsz); }
    { struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY;
      a.sin_port=htons(G.role==ROLE_ENTRY?0:(uint16_t)quic_port);
      if(bind(G.udp_fd,(struct sockaddr*)&a,sizeof(a))<0){ log4c_error("bind udp fail: %s",strerror(errno)); log4c_shutdown(); return 1; }
      set_nonblock(G.udp_fd); }
    { socklen_t ll=sizeof(G.local_addr); memset(&G.local_addr,0,sizeof(G.local_addr));
      getsockname(G.udp_fd,(struct sockaddr*)&G.local_addr,&ll);
      if(G.local_addr.ss_family==AF_INET){ struct sockaddr_in* s4=(struct sockaddr_in*)&G.local_addr;
        if(s4->sin_addr.s_addr==INADDR_ANY) s4->sin_addr.s_addr=htonl(INADDR_LOOPBACK); } }

    if(G.role==ROLE_ENTRY){
        G.tcp_listen_fd=socket(AF_INET,SOCK_STREAM,0);
        int one=1; setsockopt(G.tcp_listen_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        if(setsockopt(G.tcp_listen_fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one))!=0){
            log4c_error("tcp SO_REUSEPORT failed: %s",strerror(errno));log4c_shutdown();return 1;
        }
        struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons((uint16_t)listen_port);
        if(bind(G.tcp_listen_fd,(struct sockaddr*)&a,sizeof(a))<0){ log4c_error("bind tcp :%d fail: %s",listen_port,strerror(errno)); log4c_shutdown(); return 1; }
        listen(G.tcp_listen_fd,128); set_nonblock(G.tcp_listen_fd);
        if(entry_open_upstream()!=0){ log4c_error("entry open upstream fail"); log4c_shutdown(); return 1; }
        if(G.socks_enabled)
            log4c_info("entry up: SOCKS5 :%d -> QUIC %s:%d mid=[%s] cc=%s",listen_port,next_host,next_port,G.mid_route,
                (getenv("NB_CC")&&getenv("NB_CC")[0])?getenv("NB_CC"):"bbr");
        else
            log4c_info("entry up: TCP :%d -> QUIC %s:%d route=%s cc=%s",listen_port,next_host,next_port,route,
                (getenv("NB_CC")&&getenv("NB_CC")[0])?getenv("NB_CC"):"bbr");
    } else {
        log4c_info("%s up: QUIC :%d cc=%s",role_name(G.role),quic_port,
            (getenv("NB_CC")&&getenv("NB_CC")[0])?getenv("NB_CC"):"bbr");
    }

    if(dns_init()!=0){ log4c_error("async DNS init fail"); log4c_shutdown(); return 1; }
    G.epoll_fd=epoll_create1(EPOLL_CLOEXEC);
    if(G.epoll_fd<0){log4c_error("epoll_create1 fail: %s",strerror(errno));log4c_shutdown();return 1;}
    { char error[256];G.control_fd=nb_control_open(control_path,error,sizeof(error));
      if(G.control_fd<0){log4c_error("control socket failed: %s",error);return 1;}
      log4c_info("control socket: %s",control_path); }
    {
        struct epoll_event ev;memset(&ev,0,sizeof(ev));ev.events=EPOLLIN|EPOLLERR;
        ev.data.u64=NB_EPOLL_TAG_UDP;
        if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,G.udp_fd,&ev)!=0){log4c_error("epoll add udp fail: %s",strerror(errno));return 1;}
        ev.data.u64=NB_EPOLL_TAG_DNS;
        if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,DNS.pipe_rd,&ev)!=0){log4c_error("epoll add dns fail: %s",strerror(errno));return 1;}
        if(G.tcp_listen_fd>=0){ev.data.u64=NB_EPOLL_TAG_LISTEN;
            if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,G.tcp_listen_fd,&ev)!=0){log4c_error("epoll add listen fail: %s",strerror(errno));return 1;}}
        ev.data.u64=NB_EPOLL_TAG_CONTROL;
        if(epoll_ctl(G.epoll_fd,EPOLL_CTL_ADD,G.control_fd,&ev)!=0){log4c_error("epoll add control fail: %s",strerror(errno));return 1;}
    }
    log4c_info("event loop: epoll enabled");

    /* ---- 自定义事件循环 ---- */
    for(;;){
        uint64_t now=picoquic_current_time();
        int64_t wd=picoquic_get_next_wake_delay(G.quic,now,1000000); /* us, cap 1s */
        for(i=0;i<MAX_CONN;i++){
            proxy_stream_t* p=&G.streams[i];if(!p->in_use||p->fec_engine==NULL)continue;
            uint64_t dl=nb_fec_next_deadline(p->fec_engine);if(dl==0)continue;
            int64_t fwd=(dl<=now)?0:(int64_t)(dl-now);if(fwd<wd)wd=fwd;
        }
        int wait_ms=(wd<=0)?0:(int)((wd+999)/1000);if(wait_ms>1000)wait_ms=1000;
        struct epoll_event events[256];
        int event_count=epoll_wait(G.epoll_fd,events,(int)(sizeof(events)/sizeof(events[0])),wait_ms);
        if(event_count<0&&errno!=EINTR){log4c_error("epoll_wait fail: %s",strerror(errno));break;}
        if(event_count<0)event_count=0;
        now=picoquic_current_time();
        for(int ei=0;ei<event_count;ei++){
            uint64_t tag=events[ei].data.u64;uint32_t ee=events[ei].events;
            if(tag==NB_EPOLL_TAG_UDP){udp_drain(now);continue;}
            if(tag==NB_EPOLL_TAG_DNS){
                char drain[256];while(read(DNS.pipe_rd,drain,sizeof(drain))>0){}
                dns_res_t r;
                while(dns_pop_result(&r)){
                    proxy_stream_t* p=ps_find_by_id(r.ps_id);
                    if(!p||!p->dns_pending)continue;
                    p->dns_pending=0;
                    if(!r.ok){
                        p->target_connect_state=3;p->target_connect_done_at=picoquic_current_time();
                        log4c_warn("exit id=%u target connect result=dns-fail route=%s",p->id,p->route);
                        ps_teardown_reason(p,"target-dns-fail");continue;
                    }
                    if(p->udp_mode&&!p->udp_association){
                        int ufd=udp_connect_addr((struct sockaddr*)&r.addr,r.addrlen);
                        if(ufd<0){log4c_warn("exit udp target connect fail id=%u error=%s",p->id,strerror(errno));
                            ps_teardown_reason(p,"udp-target-connect-fail");continue;}
                        p->udp_fd=ufd;
                        if(epoll_udp_update(p,1)!=0){ps_teardown_reason(p,"udp-target-epoll-fail");continue;}
                        p->udp_target_ready_at=picoquic_current_time();
                        log4c_info("exit udp trace id=%u usid=%u stage=target-ready age=%.1fms pending=%zu target=%s:%d",
                            p->id,p->udp_session_id,trace_delta_ms(p->udp_target_ready_at,p->created_at),
                            p->udp_pending_tx_len,p->udp_target_host,p->udp_target_port);
                        while(p->udp_pending_tx_len>=2){
                            const uint8_t* pending=NULL;size_t pending_len=0;
                            if(!dgramq_peek(p->udp_pending_tx,p->udp_pending_tx_len,&pending,&pending_len))break;
                            if(send(p->udp_fd,pending,pending_len,MSG_NOSIGNAL)!=(ssize_t)pending_len){
                                ps_teardown_reason(p,"udp-target-send-fail");break;
                            }
                            if(p->udp_first_target_send_at==0){
                                p->udp_first_target_send_at=picoquic_current_time();
                                log4c_info("exit udp trace id=%u usid=%u stage=target-first-send age=%.1fms ready_to_send=%.1fms bytes=%zu target=%s:%d",
                                    p->id,p->udp_session_id,trace_delta_ms(p->udp_first_target_send_at,p->created_at),
                                    trace_delta_ms(p->udp_first_target_send_at,p->udp_target_ready_at),pending_len,
                                    p->udp_target_host,p->udp_target_port);
                            }
                            dgramq_consume(p->udp_pending_tx,&p->udp_pending_tx_len);
                        }
                        if(p->in_use)log4c_info("exit udp target ready id=%u usid=%u target=%s:%d",
                            p->id,p->udp_session_id,p->udp_target_host,p->udp_target_port);
                        continue;
                    }
                    p->target_connect_at=picoquic_current_time();p->target_connect_state=1;
                    int fd=tcp_connect_addr((struct sockaddr*)&r.addr,r.addrlen);
                    if(fd<0){
                        p->target_connect_state=3;p->target_connect_done_at=picoquic_current_time();
                        log4c_warn("exit id=%u target connect result=start-fail error=%s route=%s",p->id,strerror(errno),p->route);
                        ps_teardown_reason(p,"target-connect-start-fail");continue;
                    }
                    p->tcp_fd=fd;p->tcp_connecting=1;
                    if(epoll_tcp_update(p,1)!=0){
                        p->target_connect_state=3;p->target_connect_done_at=picoquic_current_time();
                        log4c_warn("exit id=%u target connect result=epoll-fail error=%s route=%s",p->id,strerror(errno),p->route);
                        ps_teardown_reason(p,"target-connect-epoll-fail");continue;
                    }
                    if(ps_is_rtc_webcast(p)){ char target[96]; sockaddr_to_text((struct sockaddr*)&r.addr,r.addrlen,target,sizeof(target));
                      log4c_info("exit id=%u target connect result=pending target=%s route=%s",p->id,target,p->route); }
                }
                continue;
            }
            if(tag==NB_EPOLL_TAG_LISTEN){entry_accept();continue;}
            if(tag==NB_EPOLL_TAG_CONTROL){if(nb_control_serve(G.control_fd,control_render,NULL)<0)log4c_warn("control serve failed");continue;}
            if((tag&NB_EPOLL_TAG_UDP_SESSION)!=0){
                size_t index=(size_t)(tag&0xffffu);uint32_t id=(uint32_t)((tag>>16)&0xffffffffu);
                if(index<MAX_CONN){proxy_stream_t* p=&G.streams[index];
                    if(p->in_use&&p->id==id&&p->udp_fd>=0)udp_local_drain(p);}
                continue;
            }
            if((tag&NB_EPOLL_TAG_STREAM)==0)continue;
            size_t index=(size_t)(tag&0xffffu);uint32_t id=(uint32_t)((tag>>16)&0xffffffffu);
            if(index>=MAX_CONN)continue;
            proxy_stream_t* p=&G.streams[index];
            if(!p->in_use||p->id!=id||p->tcp_fd<0)continue;
            if(p->tcp_connecting&&(ee&(EPOLLOUT|EPOLLERR|EPOLLHUP))){
                int se=0;socklen_t sl=sizeof(se);getsockopt(p->tcp_fd,SOL_SOCKET,SO_ERROR,&se,&sl);p->tcp_connecting=0;
                p->target_connect_done_at=picoquic_current_time();
                if(se!=0){p->target_connect_state=3;log4c_warn("exit id=%u target connect result=fail latency=%.1fms error=%s route=%s",
                        p->id,(p->target_connect_done_at-p->target_connect_at)/1000.0,strerror(se),p->route);
                    if(p->up_cnx)picoquic_reset_stream(p->up_cnx,p->up_stream_id,1);
                    ps_set_close_reason(p,"target-connect-fail");ps_free(p);continue;}
                p->target_connect_state=2;
                if(ps_is_rtc_webcast(p)) log4c_info("exit id=%u target connect result=ok latency=%.1fms route=%s",
                    p->id,(p->target_connect_done_at-p->target_connect_at)/1000.0,p->route);
            }
            if(p->in_use&&!p->tcp_connecting&&(ee&(EPOLLIN|EPOLLRDHUP|EPOLLHUP|EPOLLERR))){
                if(p->udp_association){
                    uint8_t control[64];ssize_t n=recv(p->tcp_fd,control,sizeof(control),0);
                    if(n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK))ps_teardown_reason(p,"udp-control-close");
                }else if(G.socks_enabled&&p->socks_stage<3)socks_handshake(p);else pump_tcp(p);
            }
            if(p->in_use&&(ee&EPOLLOUT))flush_q2t(p);
            if(p->in_use)maybe_free(p);
        }
        /* middle: 无 TCP 的流也要判释放 */
        if(G.role==ROLE_MIDDLE){ for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i]; if(p->in_use&&p->tcp_fd<0) maybe_free(p); } }
        /* V1.5 FEC sidecar: 部分 block 超时封口，避免小流量永远攒不满 K 个 symbol 而不发。 */
        { uint64_t fnow=picoquic_current_time();
          for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
            if(!p->in_use || !p->fec_sidecar_mode) continue;
            if(p->fec_engine==NULL){p->need_teardown=1;continue;}
            int rc=nb_fec_tick(p->fec_engine,fnow);
            if(rc!=NB_FEC_OK){ log4c_warn("id=%u fec engine tick fail rc=%d",p->id,rc); p->need_teardown=1; }
          } }
        /* 过载保护: q2t 超限标记的流, 主循环兜底 teardown(不在回调栈内拆, 避免重入/use-after-free) */
        for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i]; if(p->in_use && p->need_teardown) ps_teardown_reason(p,"internal-error"); }
        /* 空闲超时兜底: 无数据活动超 IDLE_TIMEOUT 的流强制拆除(防僵尸流累积拖死复用连接; 半开/对端不关 FIN 亦兜住) */
        { uint64_t tnow=picoquic_current_time();
          for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i];
            if(p->in_use && tnow > p->last_active && tnow - p->last_active > IDLE_TIMEOUT_US){
                log4c_warn("id=%u idle timeout(%llus), teardown route=%s",p->id,
                    (unsigned long long)((tnow-p->last_active)/1000000),p->route);
                ps_teardown_reason(p,"idle-timeout"); } } }
        /* 过载保护: 流表用量观测(每10s), 峰值接近 MAX_CONN 告警, 供扩容/限流决策 */
        { static uint64_t su_last=0; uint64_t tn=picoquic_current_time();
            if(tn - su_last > 10000000ULL){ su_last=tn;
                if(g_ps_peak*10 >= MAX_CONN*9) log4c_warn("stream table HIGH: inuse=%d peak=%d / max=%d",g_ps_inuse,g_ps_peak,MAX_CONN);
                else log4c_info("stream table: inuse=%d peak=%d / max=%d",g_ps_inuse,g_ps_peak,MAX_CONN); } }
        /* 白名单热重载(限流 ~5s 检查一次文件 mtime) */
        if(WL.path[0]){ static uint64_t wl_last=0; uint64_t tn=picoquic_current_time();
            if(tn - wl_last > 5000000ULL){ wl_last=tn; wl_reload_if_changed(); } }
        /* 链路质量观测(每10s): 记录到下一跳各连接的 RTT/丢包, 定位抖动/丢包在哪跳(FEC 动态冗余输入)。
         * entry 观测 entry->middle 段, middle 观测 middle->exit 段, 两段全覆盖。 */
        if(G.pool_count>0){ static uint64_t lq_last=0; uint64_t tn=picoquic_current_time();
            if(tn - lq_last > 10000000ULL){ lq_last=tn;
                for(int pool_id=0;pool_id<G.pool_count;pool_id++)for(int k=0;k<POOL_SIZE;k++){
                    cnx_pool_t* pool=&G.pools[pool_id];if(!pool->cnx[k])continue;
                    picoquic_path_quality_t q; memset(&q,0,sizeof(q));
                    picoquic_get_default_path_quality(pool->cnx[k], &q);
                    uint64_t delta_sent = (q.sent >= pool->last_sent_total[k]) ? (q.sent - pool->last_sent_total[k]) : q.sent;
                    uint64_t delta_lost = (q.lost >= pool->last_lost_total[k]) ? (q.lost - pool->last_lost_total[k]) : q.lost;
                    uint64_t delta_timer = (q.timer_losses >= pool->last_timer_total[k])
                        ? (q.timer_losses - pool->last_timer_total[k]) : q.timer_losses;
                    uint64_t delta_spurious = (q.spurious_losses >= pool->last_spurious_total[k])
                        ? (q.spurious_losses - pool->last_spurious_total[k]) : q.spurious_losses;
                    uint64_t delta_repeat = (delta_lost > delta_timer) ? (delta_lost - delta_timer) : 0;
                    uint64_t eff_lost = (delta_lost > delta_spurious) ? (delta_lost - delta_spurious) : 0;
                    picoquic_cnx_t* cnx=pool->cnx[k];
                    uint64_t delta_retrans=(cnx->nb_retransmission_total>=pool->last_retrans_total[k])
                        ? cnx->nb_retransmission_total-pool->last_retrans_total[k] : cnx->nb_retransmission_total;
                    uint64_t delta_preempt=(cnx->nb_preemptive_repeat>=pool->last_preempt_total[k])
                        ? cnx->nb_preemptive_repeat-pool->last_preempt_total[k] : cnx->nb_preemptive_repeat;
                    uint64_t cc_state=0,cc_param=0;
                    const char* cc_alg=(cnx->congestion_alg&&cnx->congestion_alg->congestion_algorithm_id)
                        ? cnx->congestion_alg->congestion_algorithm_id : "none";
                    picoquic_path_t* path=(cnx->nb_paths>0)?cnx->path[0]:NULL;
                    if(path&&path->congestion_alg_state&&cnx->congestion_alg&&cnx->congestion_alg->alg_observe)
                        cnx->congestion_alg->alg_observe(path,&cc_state,&cc_param);
                    double loss = delta_sent ? (100.0*(double)eff_lost/(double)delta_sent) : 0.0;
                    pool->recent_loss[k] = loss;
                    pool->recent_rtt[k] = q.rtt;
                    pool->recent_rtt_max[k] = q.rtt_max;
                    pool->recent_sent[k] = delta_sent;
                    pool->last_sent_total[k] = q.sent;
                    pool->last_lost_total[k] = q.lost;
                    pool->last_timer_total[k] = q.timer_losses;
                    pool->last_spurious_total[k] = q.spurious_losses;
                    pool->last_retrans_total[k] = cnx->nb_retransmission_total;
                    pool->last_preempt_total[k] = cnx->nb_preemptive_repeat;
                    pool->recent_rtt_var[k] = q.rtt_variant;
                    pool->recent_ts[k] = tn;
                    log4c_info("linkq pool[%d:%d] rtt=%.1fms(min%.1f/max%.1f) jit=%.1fms loss=%.2f%% dsent=%llu loss_raw=%llu eff=%llu spur=%llu timer=%llu repeat=%llu retx=%llu/%llu pre=%llu/%llu reorder=%.1fms/%llu tol=%llu/%.1fms ack=%.1fms(min%.1f/max%.1f) cc=%s:%s(%llu)/%llu cwin=%lluKB bif=%lluKB block=%u/%u/%u bw=%lluKbps sent=%lluKB",
                        pool_id,k,q.rtt/1000.0, q.rtt_min/1000.0, q.rtt_max/1000.0,
                        (double)q.rtt_variant/1000.0, loss,
                        (unsigned long long)delta_sent,
                        (unsigned long long)delta_lost,
                        (unsigned long long)eff_lost,
                        (unsigned long long)delta_spurious,
                        (unsigned long long)delta_timer,
                        (unsigned long long)delta_repeat,
                        (unsigned long long)delta_retrans,(unsigned long long)cnx->nb_retransmission_total,
                        (unsigned long long)delta_preempt,(unsigned long long)cnx->nb_preemptive_repeat,
                        q.max_reorder_delay/1000.0,(unsigned long long)q.max_reorder_gap,
                        (unsigned long long)((cnx->loss_reorder_gap>=3)?cnx->loss_reorder_gap:3),cnx->loss_reorder_delay/1000.0,
                        cnx->ack_delay_remote/1000.0,cnx->min_ack_delay_remote/1000.0,cnx->max_ack_delay_remote/1000.0,
                        cc_alg,cc_state_name(cc_alg,cc_state),(unsigned long long)cc_state,(unsigned long long)cc_param,
                        (unsigned long long)(q.cwin/1024),
                        (unsigned long long)(q.bytes_in_transit/1024),
                        cnx->cwin_blocked,cnx->flow_blocked,cnx->stream_blocked,
                        (unsigned long long)(q.pacing_rate*8/1000),
                        (unsigned long long)(q.bytes_sent/1024));
                }
            }
        }
        /* 4) QUIC -> UDP (GSO 批量发送, 降 syscall; 不支持时逐段回退) */
        if(g_fec_v15_observe || g_fec_v15_active){
            static uint64_t fecv15_last=0; uint64_t ftn=picoquic_current_time();
            if(ftn - fecv15_last > 10000000ULL){ fecv15_last=ftn;
                uint64_t ctrl_open=0, ctrl_ready=0, dgq=0, dgs=0, dgr=0, dga=0, dgl=0, stageq=0;
                nb_fec_metrics_t fm=g_fec_metrics_done;
                for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
                    if(!p->in_use || p->fec_session_id==0) continue;
                    ctrl_open += p->fec_ctrl_opened != 0;
                    ctrl_ready += p->fec_ctrl_peer_ready != 0;
                    dgq += (uint64_t)p->fec_dg_tx_len;
                    stageq += (uint64_t)p->fec_stage_tx_len;
                    dgs += p->fec_dg_sent;
                    dgr += p->fec_dg_recv;
                    dga += p->fec_dg_acked;
                    dgl += p->fec_dg_lost;
                    if(p->fec_engine!=NULL) fec_metrics_add(&fm,nb_fec_metrics(p->fec_engine));
                }
                if(ctrl_open || ctrl_ready || dgq || stageq || dgs || dgr || dga || dgl || fm.blocks_encoded || fm.blocks_delivered || g_fec_v15_drop_src || g_fec_v15_drop_repair){
                    double overhead=fm.source_bytes?100.0*(double)fm.repair_bytes/(double)fm.source_bytes:0.0;
                    log4c_info("fec v15 stat: ctrl_open=%llu ctrl_ready=%llu stage_queue=%lluB dg_queue=%lluB sent=%llu recv=%llu dg_acked=%llu dg_lost=%llu encoded=%llu delivered=%llu recovered=%llu recovered_bytes=%llu source_bytes=%llu repair_bytes=%llu overhead=%.1f%% nack=%llu nack_retry=%llu retx_sent=%llu retx_recv=%llu retx_acked=%llu block_acked=%llu ooo=%llu dup=%llu proto_err=%llu window_err=%llu retry_exhausted=%llu drop_src=%llu drop_repair=%llu",
                        (unsigned long long)ctrl_open, (unsigned long long)ctrl_ready,
                        (unsigned long long)stageq, (unsigned long long)dgq,
                        (unsigned long long)dgs, (unsigned long long)dgr, (unsigned long long)dga, (unsigned long long)dgl,
                        (unsigned long long)fm.blocks_encoded,(unsigned long long)fm.blocks_delivered,
                        (unsigned long long)fm.blocks_recovered,(unsigned long long)fm.recovered_source_bytes,
                        (unsigned long long)fm.source_bytes,(unsigned long long)fm.repair_bytes,overhead,
                        (unsigned long long)fm.nack_sent,(unsigned long long)fm.nack_retried,
                        (unsigned long long)fm.retx_sent,(unsigned long long)fm.retx_received,(unsigned long long)fm.retx_acked,
                        (unsigned long long)fm.block_acked,(unsigned long long)fm.out_of_order_packets,
                        (unsigned long long)fm.duplicate_packets,(unsigned long long)fm.protocol_errors,
                        (unsigned long long)fm.window_errors,(unsigned long long)fm.retry_exhausted,
                        (unsigned long long)g_fec_v15_drop_src, (unsigned long long)g_fec_v15_drop_repair);
                }
            }
        }
        udp_send_batch(now);
        /* 发送和 FEC tick 可能改变队列水位，循环末统一刷新 TCP EPOLLIN/EPOLLOUT。 */
        for(i=0;i<MAX_CONN;i++){proxy_stream_t* p=&G.streams[i];if(p->in_use&&p->tcp_fd>=0)(void)epoll_tcp_update(p,0);}
    }
    log4c_shutdown();
    return 0;
}
