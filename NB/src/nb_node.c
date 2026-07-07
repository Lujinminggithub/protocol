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
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <picoquic.h>
#include <picosocks.h>
#include <picoquic_utils.h>
#include <picoquic_bbr.h>
#include "log/log4c.h"

#define NB_ALPN "nb/1"          /* Newbility(NB) 新传输协议 ALPN */
#define NB_SNI  "nb.internal"
#define BUFCAP (256*1024)
#define MAX_CONN 1024
#define NB_Q2T_MAX (16*1024*1024)   /* 过载保护: 单流 q2t 待写缓冲硬上限 16MB, 超限=对端TCP卡死 */
#define IDLE_TIMEOUT_US (120*1000000ULL)  /* 流空闲(无数据)超时兜底回收, 防僵尸流累积拖死复用连接 */
#define POOL_SIZE 6                        /* 到下一跳的并行 QUIC 连接数(消除单连接 cwnd/pacing/flow-control 共享瓶颈) */
#define LAT_LANES 3                        /* 前 LAT_LANES 条为直播/实时专用道(与批量物理隔离, 内部再 round-robin 减竞争) */
/* 流优先级(picoquic: 0-255, 小=高, 偶数=round-robin)。直播/实时走低延迟专用道, 批量下载降级,
 * 避免视频预加载的大流量把直播小包挤在发送队列后面(bufferbloat)导致直播卡顿。 */
#define PRIO_LATENCY 4                      /* 直播/实时(音视频): 高优先 round-robin */
#define PRIO_BULK    20                     /* 批量下载: 低优先 round-robin */

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
    int down_opened;             /* 下游 stream 首部已发 */
    /* TCP: entry=客户端 conn; exit=目标 conn; -1=无 */
    int tcp_fd;
    int tcp_connecting;          /* exit: 非阻塞 connect 进行中 */
    /* 首部解析(middle/exit 收 up 首部) */
    int hdr_done;
    char hdr[600];
    size_t hdr_len;
    /* q2t: 面向 TCP 的待写缓冲(entry 写回客户端 / exit 写目标)。动态增长, 永不丢数据;
     * 总量上限由 QUIC connection flow control(max_data) 自然背压 */
    uint8_t* q2t;
    size_t q2t_len;
    size_t q2t_cap;
    int q2t_fin;                 /* 待对 TCP 做半关 */
    int need_teardown;           /* 过载保护: q2t 超限, 主循环兜底回收(不在回调栈内拆) */
    int tcp_eof;                 /* 本地 TCP 读到 EOF */
    /* fin 追踪(去程 left->right, 回程 right->left) */
    int up_fin_seen;             /* 收到 left QUIC fin(仅 middle/exit) */
    int down_fin_seen;           /* 收到 right QUIC fin(仅 entry/middle) */
    char route[300];             /* 本 stream 的 route 首部(日志) */
    uint64_t last_active;        /* 最近一次有数据活动的时刻(us), 空闲超时兜底回收用 */
    int dns_pending;             /* exit: 该流的 target 域名正在异步 DNS 解析中 */
    int target_port;             /* exit: DNS 解析完成后 connect 用的端口 */
    int prio;                    /* 流优先级(PRIO_LATENCY/PRIO_BULK), 首部 <prio>; 端到端传递 */
    /* ===== 双发去重(方案A, 抗随机丢包) =====
     * 逻辑流在"瓶颈跳"用 2 条 QUIC stream(分属 pool 中 2 条不同连接)承载同一份数据,
     * 接收端按 flowid 把 2 条 stream 归到同一 proxy_stream, 按字节去重只交付一次。
     * 两条均是可靠有序 stream, 任一条即可完整送达 -> 永不损坏(无需 ARQ), 丢包时另一条
     * 可能已到 -> 消除重传延迟毛刺。首部 <prio>;<flowid>;route, flowid=0 = 单发(走现状)。
     * 机制角色对称: 任何有下游 pool 的节点(entry/middle)都能双开下游; 任何 server 节点
     * (middle/exit)都能对上游配对去重。产品化: 每跳按本跳 linkq 自适应决定是否开双发。 */
    uint32_t flowid;             /* 本逻辑流双发组 ID(发送方为该跳分配, 0=单发) */
    picoquic_cnx_t* down2_cnx;   /* 双发第 2 条下游 stream(与 down_cnx 不同连接; NULL=未双发) */
    uint64_t down2_stream_id;
    int down2_opened;
    picoquic_cnx_t* up2_cnx;     /* 双发第 2 条上游 stream(接收端配对到同一 ps; NULL=未配对) */
    uint64_t up2_stream_id;
    uint64_t dedup_delivered;    /* 去重: 该逻辑流已向 TCP/下游交付的字节数 */
    uint64_t up_rcvd, up2_rcvd;  /* 接收端每条物理 up stream 的累计已收字节(去重进度基准) */
    uint64_t down_rcvd, down2_rcvd; /* 回程(middle)每条物理 down stream 累计已收字节(去重基准) */
    uint64_t dedup_upstreamed;      /* 回程去重: 已向上游转发的字节数 */
    int up_fin_sent;                /* 回程: 已向上游发过 fin(两条 down 各触发一次, 防重复) */
    int down_fin_sent;              /* 去程(entry->middle FEC): 已向下游发过 fin(两条 up 各触发一次, 防重复) */
    /* SOCKS5 入口(仅 entry -S): 握手阶段 0=greeting 1=request 2=直通 */
    int socks_stage;
    uint8_t socks_buf[512];
    size_t socks_len;
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
    uint64_t last_spurious_total[POOL_SIZE]; /* 上次采样时的累计 spurious_losses */
    uint64_t recent_rtt_var[POOL_SIZE]; /* 最近一次采样窗对应的 RTT 抖动(us) */
    uint64_t recent_ts[POOL_SIZE];   /* 最近一次 linkq 采样时间(us), 0=暂无样本 */
    int fec_latched;                 /* D 方案回滞: 当前是否保持双发开启态 */
} cnx_pool_t;

typedef struct nb_global {
    nb_role_t role;
    picoquic_quic_t* quic;
    int udp_fd;                          /* QUIC UDP socket */
    struct sockaddr_storage local_addr;  /* incoming_packet 的 addr_to */
    /* entry: 本地 TCP listen */
    int tcp_listen_fd;
    char route_str[300];                 /* entry: 发给第一跳的 route(固定 target 模式) */
    int socks_enabled;                   /* entry: SOCKS5 入口(动态 target) */
    char mid_route[256];                 /* entry SOCKS 模式: 中间跳前缀 "H:kz:4443"(可空=两跳) */
    /* entry/middle: 到下一跳的 QUIC 连接池(每条独立 cwnd/pacing/flow-control, round-robin 分流) */
    cnx_pool_t pool;
    struct sockaddr_storage next_addr;    /* entry: 第一跳地址(启动配, pool_init 用) */
    int next_configured;                  /* entry: next_addr 已配 */
    uint32_t next_ps_id;                  /* proxy_stream 流水号分配 */
    proxy_stream_t streams[MAX_CONN];
} nb_global_t;

static nb_global_t G;
static int g_ps_inuse=0, g_ps_peak=0;   /* 过载保护: 在用流数 + 峰值 */
static uint32_t g_next_flowid=0;        /* FEC 双发: 逻辑流双发组 ID 分配(从1递增, 0=单发) */
static uint64_t g_fec_flow_started=0;    /* FEC观测:成功开启双发的逻辑流数 */
static uint64_t g_fec_fallback_single=0; /* FEC观测:该双发却退化单发的次数 */
static uint64_t g_fec_dedup_bytes=0;     /* FEC观测:去重省下的重复字节(收端累计) */
static int g_fec_enabled=0;             /* FEC 双发总开关(NB_FEC=on/1 启用; 默认关=单发, 行为同未启用) */

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
        memset(&hints,0,sizeof(hints)); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
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
/* Q1: 把明显的直播/实时控制流 + 一小批已证实会承载直播推流的媒体 CDN 送进低延迟专用道。
 * 这里不再追求完整媒体识别, 只保守抬高那批"开播时不抬优先级就容易炸"的域名模式。 */
static int classify_target(const char* host){
    static const char* pat[]={
        "webcast","pull","live","flv","rtmp","rtc","rtcpc","frontier","netacc",
        "tiktokcdn","teko","sf-teko","v16m","v58","v9","common-sign","pitayacdn","ies-music",
    };
    for(unsigned i=0;i<sizeof(pat)/sizeof(pat[0]);i++) if(strstr(host,pat[i])) return PRIO_LATENCY;
    return PRIO_BULK;
}


/* ===== 白名单(访问控制) =====
 * entry 收到 SOCKS CONNECT(host:port): 命中(域名后缀 / IP CIDR / 端口)才允许走隧道, 未命中直接拒绝。
 * 目的: 防止无关流量走三跳线路占带宽 + 降低安全风险。启动加载 + 文件 mtime 热重载。entry 单线程, 无锁。
 * 规则: host命中(域名或IP) AND port命中; 某类白名单为空 = 该维度不限制。 */
#define WL_MAX 1024
typedef struct { uint32_t net, mask; } wl_cidr_t;
static struct {
    char domain[WL_MAX][128]; int n_dom;   /* 域名后缀 */
    wl_cidr_t ip[WL_MAX];     int n_ip;    /* IPv4 CIDR */
    uint16_t port[WL_MAX];    int n_port;  /* 端口 */
    int enabled;                           /* 有配置文件才启用 */
    char path[256];
    time_t mtime;
} WL;

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
static void wl_load(const char* path){
    FILE* f=fopen(path,"r");
    if(!f){ log4c_warn("whitelist %s open fail -> allow-all",path); WL.enabled=0; return; }
    WL.n_dom=WL.n_ip=WL.n_port=0;
    char line[512];
    while(fgets(line,sizeof(line),f)){
        char* p=line; while(*p==' '||*p=='\t')p++;
        if(*p=='#'||*p=='\n'||*p=='\r'||*p==0) continue;
        char kw[16], val[256];
        if(sscanf(p,"%15s %255s",kw,val)!=2) continue;
        if(!strcmp(kw,"domain")){ if(WL.n_dom<WL_MAX) snprintf(WL.domain[WL.n_dom++],128,"%s",val); }
        else if(!strcmp(kw,"port")){ if(WL.n_port<WL_MAX) WL.port[WL.n_port++]=(uint16_t)atoi(val); }
        else if(!strcmp(kw,"ip")){
            int len=32; char* slash=strchr(val,'/'); if(slash){ *slash=0; len=atoi(slash+1); }
            struct in_addr a;
            if(inet_pton(AF_INET,val,&a)==1 && len>=0 && len<=32 && WL.n_ip<WL_MAX){
                uint32_t mask = len==0?0u:(0xFFFFFFFFu << (32-len));
                WL.ip[WL.n_ip].mask=mask; WL.ip[WL.n_ip].net=ntohl(a.s_addr)&mask; WL.n_ip++;
            }
        }
    }
    fclose(f);
    WL.enabled=1;
    log4c_info("whitelist loaded: %d domains, %d cidrs, %d ports (%s)",WL.n_dom,WL.n_ip,WL.n_port,path);
}
static void wl_init(const char* path){
    if(!path||!path[0]) return;
    snprintf(WL.path,sizeof(WL.path),"%s",path);
    struct stat st; if(stat(path,&st)==0) WL.mtime=st.st_mtime;
    wl_load(path);
}
static void wl_reload_if_changed(void){
    if(!WL.path[0]) return;
    struct stat st;
    if(stat(WL.path,&st)==0 && st.st_mtime!=WL.mtime){ WL.mtime=st.st_mtime; wl_load(WL.path); }
}

static proxy_stream_t* ps_alloc(void){
    int i; for(i=0;i<MAX_CONN;i++) if(!G.streams[i].in_use){
        memset(&G.streams[i],0,sizeof(G.streams[i]));
        G.streams[i].in_use=1; G.streams[i].tcp_fd=-1; G.streams[i].id=++G.next_ps_id;
        G.streams[i].last_active=picoquic_current_time();
        g_ps_inuse++; if(g_ps_inuse>g_ps_peak) g_ps_peak=g_ps_inuse;
        return &G.streams[i]; }
    return NULL;
}
static void ps_touch(proxy_stream_t* p){ p->last_active=picoquic_current_time(); }
/* 上游匹配: 兼配 FEC 第二条上游 stream(up2), 使双发两条 stream 归到同一 proxy_stream */
static proxy_stream_t* ps_find_by_up(picoquic_cnx_t* cnx, uint64_t sid){
    int i; for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
        if(p->in_use && ((p->up_cnx==cnx && p->up_stream_id==sid) || (p->up2_cnx==cnx && p->up2_stream_id==sid))) return p; }
    return NULL;
}
/* 下游匹配: 兼配 FEC 第二条下游 stream(down2), 使 reset/close 能正确定位 ps */
static proxy_stream_t* ps_find_by_down(picoquic_cnx_t* cnx, uint64_t sid){
    int i; for(i=0;i<MAX_CONN;i++){ proxy_stream_t* p=&G.streams[i];
        if(p->in_use && ((p->down_cnx==cnx && p->down_stream_id==sid) || (p->down2_cnx==cnx && p->down2_stream_id==sid))) return p; }
    return NULL;
}
/* FEC 配对: 按双发组 flowid 找已存在的 proxy_stream(0=单发不参与) */
static proxy_stream_t* ps_find_by_flowid(uint32_t flowid){
    if(flowid==0) return NULL;
    int i; for(i=0;i<MAX_CONN;i++) if(G.streams[i].in_use && G.streams[i].flowid==flowid) return &G.streams[i];
    return NULL;
}
static proxy_stream_t* ps_find_by_id(uint32_t id){
    int i; for(i=0;i<MAX_CONN;i++) if(G.streams[i].in_use && G.streams[i].id==id) return &G.streams[i];
    return NULL;
}
static void ps_free(proxy_stream_t* p){
    if(!p->in_use) return;               /* 幂等: 防拆流/回调重入导致 double free */
    log4c_debug("ps free id=%u up_sid=%llu down_sid=%llu route=%s", p->id,
        (unsigned long long)p->up_stream_id, (unsigned long long)p->down_stream_id, p->route);
    if(p->tcp_fd>=0) close(p->tcp_fd);
    free(p->q2t); p->q2t=NULL;
    if(g_ps_inuse>0) g_ps_inuse--;
    p->in_use=0;
}
/* 主动拆流: discard 两侧 QUIC stream(=reset+stop_sending), 连锁通知上/下游一并回收, 再释放本地。
 * 用于"本地 TCP 端已关闭 / 空闲超时"等一侧先结束的场景, 不再死等对端 FIN。 */
static void ps_teardown(proxy_stream_t* p){
    if(!p->in_use) return;
    if(p->down_cnx && p->down_opened) picoquic_discard_stream(p->down_cnx, p->down_stream_id, 0);
    if(p->down2_cnx && p->down2_opened) picoquic_discard_stream(p->down2_cnx, p->down2_stream_id, 0);
    if(p->up_cnx) picoquic_discard_stream(p->up_cnx, p->up_stream_id, 0);
    if(p->up2_cnx) picoquic_discard_stream(p->up2_cnx, p->up2_stream_id, 0);
    ps_free(p);
}

/* q2t 动态追加(永不丢数据; 背压靠 QUIC connection flow control 限总量) */
static int q2t_append(proxy_stream_t* p, const uint8_t* d, size_t n){
    if(n==0) return 0;
    if(p->q2t_len+n > NB_Q2T_MAX){   /* 过载保护: 待写超上限, 拒新数据 -> 触发回收, 防内存失控 */
        log4c_warn("id=%u q2t over limit %zu+%zu > %d, teardown",p->id,p->q2t_len,n,NB_Q2T_MAX); return -1; }
    if(p->q2t_len+n > p->q2t_cap){
        size_t ncap = p->q2t_cap ? p->q2t_cap*2 : 65536;
        while(ncap < p->q2t_len+n) ncap*=2;
        uint8_t* nb = realloc(p->q2t, ncap);
        if(!nb){ log4c_error("id=%u q2t realloc %zu fail",p->id,ncap); return -1; }
        p->q2t=nb; p->q2t_cap=ncap;
    }
    memcpy(p->q2t+p->q2t_len, d, n); p->q2t_len+=n;
    return 0;
}

/* 非阻塞 connect 目标 TCP(exit): 用已(异步)解析好的地址, 不在事件循环内做阻塞 DNS */
static int tcp_connect_addr(struct sockaddr* sa, socklen_t sl){
    int fd=socket(sa->sa_family,SOCK_STREAM,0);
    if(fd<0) return -1;
    set_nonblock(fd); int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    if(connect(fd,sa,sl)<0 && errno!=EINPROGRESS){ close(fd); return -1; }
    return fd;
}

/* 前置声明 */
static int relay_quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t ev, void* cb_ctx, void* v_stream_ctx);

/* 确保池中 idx 槽有一条已启动的连接(空槽则新建 + start + 保活)。返回 0=可用 -1=失败 */
static int pool_ensure(cnx_pool_t* pool, int idx){
    if(pool->cnx[idx]!=NULL) return 0;
    picoquic_cnx_t* c=picoquic_create_cnx(G.quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&pool->addr, picoquic_current_time(), 0, NB_SNI, NB_ALPN, 1);
    if(c==NULL){ log4c_error("pool[%d] create cnx fail",idx); return -1; }
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
    memset(pool->last_spurious_total, 0, sizeof(pool->last_spurious_total));
    memset(pool->recent_rtt_var, 0, sizeof(pool->recent_rtt_var));
    memset(pool->recent_ts, 0, sizeof(pool->recent_ts));
    pool->fec_latched = 0;
    int ok=0; for(int i=0;i<POOL_SIZE;i++) if(pool_ensure(pool,i)==0) ok++;
    pool->configured=1;
    log4c_info("cnx pool ready: %d/%d connections up",ok,POOL_SIZE);
    return ok>0?0:-1;
}

static int pool_pick_latency_rr(cnx_pool_t* pool, int avoid_idx){
    for(int k=0;k<LAT_LANES;k++){
        int idx=pool->rr_lat % LAT_LANES; pool->rr_lat++;
        if(idx==avoid_idx) continue;
        if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
        if(pool->cnx[idx]!=NULL) return idx;
    }
    for(int k=0;k<POOL_SIZE;k++){
        if(k==avoid_idx) continue;
        if(pool->cnx[k]==NULL) pool_ensure(pool,k);
        if(pool->cnx[k]!=NULL) return k;
    }
    return -1;
}

#define FEC_LOSS_ON_PCT 3.0
#define FEC_LOSS_OFF_PCT 1.0
#define FEC_JITTER_ON_US 40000ULL
#define FEC_JITTER_OFF_US 15000ULL
#define FEC_MIN_SENT_PKTS 20ULL
#define FEC_COLDSTART_US 3000000ULL

static int pool_quality_fresh(cnx_pool_t* pool, int idx, uint64_t now){
    return pool->recent_ts[idx] != 0 && now >= pool->recent_ts[idx] &&
        now - pool->recent_ts[idx] <= 30000000ULL; /* 30s 内样本视为新鲜 */
}

static int pool_quality_sampled(cnx_pool_t* pool, int idx, uint64_t now){
    return pool_quality_fresh(pool, idx, now) && pool->recent_sent[idx] >= FEC_MIN_SENT_PKTS;
}

static int pool_quality_better(cnx_pool_t* pool, int a, int b, uint64_t now){
    int af = pool_quality_fresh(pool, a, now), bf = pool_quality_fresh(pool, b, now);
    if(af != bf) return af > bf;
    if(!af && !bf) return a < b;
    if(pool->recent_loss[a] != pool->recent_loss[b]) return pool->recent_loss[a] < pool->recent_loss[b];
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
        if(prio == PRIO_LATENCY){
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
        if(prio == PRIO_LATENCY && pool->recent_ts[eval_idx] != 0 &&
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

/* FEC 2B: 从全池里挑"当前最稳"的连接。FEC 的候选不受 LAT_LANES 限制, 因为实测最稳的连接
 * 未必落在前几条 latency lanes。优先使用 30s 内的 linkq 样本按 loss->rtt 排序; 若都无样本,
 * 回退普通 latency round-robin。avoid_idx>=0 时排除该连接, 便于挑第二条。 */
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
    return best>=0 ? best : pool_pick_latency_rr(pool, avoid_idx);
}

/* 从池选一条连接。分区: 直播/实时(latency)流走前 LAT_LANES 条专用道(内部 round-robin 减少直播
 * 流之间的 cwnd 竞争), 与批量下载物理隔离; 批量流走 pool[LAT_LANES..] round-robin。
 * 空槽惰性重建, 无可用槽则回退任意槽。返回 idx 或 -1 */
static int pool_pick(cnx_pool_t* pool, int is_latency){
    if(is_latency){
        return pool_pick_latency_rr(pool, -1);
    } else if(POOL_SIZE>LAT_LANES){
        for(int k=LAT_LANES;k<POOL_SIZE;k++){
            int idx=LAT_LANES+(pool->rr % (POOL_SIZE-LAT_LANES)); pool->rr++;
            if(pool->cnx[idx]==NULL) pool_ensure(pool,idx);
            if(pool->cnx[idx]!=NULL) return idx;
        }
    }
    for(int k=0;k<POOL_SIZE;k++){ if(pool->cnx[k]==NULL) pool_ensure(pool,k); if(pool->cnx[k]!=NULL) return k; }
    return -1;
}

/* 分配下游 stream 并发首部(带 <prio>; 标记端到端传递)+ 设该 stream 优先级。 */
static int downstream_open_stream(proxy_stream_t* p, const char* route, int prio){
    /* FEC 双发(仅 middle, 开关开, 直播延迟流): 分配非0 flowid, 稍后在池中另一条连接上开 down2,
     * 首部同 flowid 端到端传, exit 端据 flowid 配对同一逻辑流并按字节去重。0=单发不受影响。 */
    double best_loss=-1.0, best_jitter_ms=-1.0;
    int best_idx=-1;
    int coldstart_on=0;
    int line_bad = ((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) && g_fec_enabled)
        ? pool_line_bad(&G.pool, prio, &best_loss, &best_jitter_ms, &best_idx, &coldstart_on) : 0;
    int fec = ((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) && g_fec_enabled && prio==PRIO_LATENCY && line_bad);
    if((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) && prio==PRIO_LATENCY){
        log4c_info("%s id=%u fec gate route=%s prio=%d fec_enabled=%d line_bad=%d coldstart=%d best=%d best_loss=%.2f best_jitter=%.1fms fec=%d",
            role_name(G.role), p->id, route, prio, g_fec_enabled, line_bad, coldstart_on, best_idx, best_loss, best_jitter_ms, fec);
    }
    if(fec && p->flowid==0){ if(++g_next_flowid==0) g_next_flowid=1; p->flowid=g_next_flowid; }

    int idx = fec ? pool_pick_fec_best(&G.pool, -1) : pool_pick(&G.pool, prio==PRIO_LATENCY);
    if(idx<0){ log4c_error("id=%u no downstream cnx in pool", p->id); return -1; }
    picoquic_cnx_t* c=G.pool.cnx[idx];
    if((G.role==ROLE_MIDDLE || G.role==ROLE_ENTRY) && g_fec_enabled && prio==PRIO_LATENCY && !line_bad){
        log4c_info("%s id=%u fec skip route=%s prio=%d line_bad=0 coldstart=%d best=%d best_loss=%.2f best_jitter=%.1fms",
            role_name(G.role), p->id, route, prio, coldstart_on, best_idx, best_loss, best_jitter_ms);
    }
    p->down_cnx=c; p->prio=prio;
    p->down_stream_id=G.pool.next_sid[idx]; G.pool.next_sid[idx]+=4; /* 该连接独立的 client bidi */
    picoquic_set_app_stream_ctx(c,p->down_stream_id,p);
    picoquic_set_stream_priority(c,p->down_stream_id,(uint8_t)prio);
    char hdr[340]; int hl=snprintf(hdr,sizeof(hdr),"%d;%u;%s\n",prio,p->flowid,route);
    if(picoquic_add_to_stream(c,p->down_stream_id,(uint8_t*)hdr,(size_t)hl,0)!=0){
        log4c_error("id=%u add route to downstream fail", p->id); return -1; }
    picoquic_mark_active_stream(c,p->down_stream_id,1,p);
    p->down_opened=1;

    /* FEC 第二条: 选一条与 down 不同的池连接开 down2, 首部同 flowid。第二条失败则退化单发, 不影响主路。 */
    if(fec && p->flowid!=0){
        int idx2=pool_pick_fec_best(&G.pool, idx);
        log4c_info("%s id=%u fec decide route=%s line_bad=1 pick=%d,%d loss=%.2f/%.2f rtt=%.1f/%.1fms",
            role_name(G.role), p->id, route, idx, idx2,
            G.pool.recent_loss[idx], idx2>=0 ? G.pool.recent_loss[idx2] : -1.0,
            G.pool.recent_rtt[idx]/1000.0, idx2>=0 ? (G.pool.recent_rtt[idx2]/1000.0) : -1.0);
        if(idx2>=0 && G.pool.cnx[idx2]!=NULL && G.pool.cnx[idx2]!=c){
            picoquic_cnx_t* c2=G.pool.cnx[idx2];
            p->down2_cnx=c2;
            p->down2_stream_id=G.pool.next_sid[idx2]; G.pool.next_sid[idx2]+=4;
            picoquic_set_app_stream_ctx(c2,p->down2_stream_id,p);
            picoquic_set_stream_priority(c2,p->down2_stream_id,(uint8_t)prio);
            char h2[340]; int hl2=snprintf(h2,sizeof(h2),"%d;%u;%s\n",prio,p->flowid,route);
            if(picoquic_add_to_stream(c2,p->down2_stream_id,(uint8_t*)h2,(size_t)hl2,0)==0){
                picoquic_mark_active_stream(c2,p->down2_stream_id,1,p);
                p->down2_opened=1; g_fec_flow_started++;
                log4c_debug("middle id=%u FEC dual-send flowid=%u down_sid=%llu down2_sid=%llu",
                    p->id,p->flowid,(unsigned long long)p->down_stream_id,(unsigned long long)p->down2_stream_id);
            } else { p->down2_cnx=NULL; log4c_warn("middle id=%u FEC down2 add hdr fail, single-send",p->id); }
        }
    }
    if(fec && p->flowid!=0 && !p->down2_opened) g_fec_fallback_single++;  /* 覆盖 idx2<0/无第二条/add失败 所有退化 */
    return 0;
}

/* 去程转发: left -> right */
static void fwd_down(proxy_stream_t* p, uint8_t* d, size_t n, int fin){
    ps_touch(p);
    if(G.role==ROLE_EXIT){
        if(q2t_append(p,d,n)!=0){ p->need_teardown=1; return; }
        if(fin) p->q2t_fin=1;
    } else {
        if(n||fin){ picoquic_add_to_stream(p->down_cnx,p->down_stream_id,d,n,fin);
            picoquic_mark_active_stream(p->down_cnx,p->down_stream_id,1,p);
            if(p->down2_cnx && p->down2_opened){   /* FEC 双写第二条相同字节 */
                picoquic_add_to_stream(p->down2_cnx,p->down2_stream_id,d,n,fin);
                picoquic_mark_active_stream(p->down2_cnx,p->down2_stream_id,1,p); } }
    }
}
/* 回程转发: right -> left */
static void fwd_up(proxy_stream_t* p, uint8_t* d, size_t n, int fin){
    ps_touch(p);
    if(G.role==ROLE_ENTRY){
        if(q2t_append(p,d,n)!=0){ p->need_teardown=1; return; }
        if(fin) p->q2t_fin=1;
    } else { /* middle / exit: 写回上游 QUIC stream */
        if(n||fin){ picoquic_add_to_stream(p->up_cnx,p->up_stream_id,d,n,fin);
            picoquic_mark_active_stream(p->up_cnx,p->up_stream_id,1,p);
            if(G.role==ROLE_EXIT && p->up2_cnx){   /* FEC 回程双写第二条上游(up2 配对必早于 target 回程首字节) */
                picoquic_add_to_stream(p->up2_cnx,p->up2_stream_id,d,n,fin);
                picoquic_mark_active_stream(p->up2_cnx,p->up2_stream_id,1,p); } }
    }
}

/* middle 回程 FEC 去重: exit 双发两条下游 stream(down/down2)承载相同回程字节, middle 按来源累计,
 * 只把新增(超 dedup_upstreamed)部分转发给上游 -> entry 收单份。单发(无 down2, from_down2 恒0)时
 * down_rcvd 单调等于 dedup_upstreamed, 等价直接转发。任一条 fin 即数据完整, 仅发一次上游 fin。 */
static void upstream_dedup(proxy_stream_t* p, int from_down2, uint8_t* data, size_t n, int fin){
    ps_touch(p);
    uint64_t* rcvd = from_down2 ? &p->down2_rcvd : &p->down_rcvd;
    uint64_t now = *rcvd + n;
    *rcvd = now;
    if(now > p->dedup_upstreamed){
        uint64_t nb = now - p->dedup_upstreamed;
        if(nb > n) nb = n;
        fwd_up(p, data + (n - nb), (size_t)nb, 0);
        p->dedup_upstreamed = now;
        g_fec_dedup_bytes += (uint64_t)(n - nb);
    } else if(n){
        g_fec_dedup_bytes += (uint64_t)n;
    }
    if(fin && !p->up_fin_sent){ p->up_fin_sent=1; fwd_up(p, NULL, 0, 1); }
}

/* middle 去程 FEC 去重: entry 双发两条上游 stream(up/up2)承载相同去程字节流。middle 只把新增部分
 * 转发到下游(此时 fwd_down 仍可继续在 middle->exit 段做下一跳双发), 避免 entry 双发放大成 2x 以上。
 * 任一条 fin 只向下游发一次。 */
static void middle_deliver_dedup(proxy_stream_t* p, int from_up2, uint8_t* data, size_t n, int fin){
    ps_touch(p);
    uint64_t* rcvd = from_up2 ? &p->up2_rcvd : &p->up_rcvd;
    uint64_t now = *rcvd + n;
    *rcvd = now;
    if(now > p->dedup_delivered){
        uint64_t nb = now - p->dedup_delivered;
        if(nb > n) nb = n;
        fwd_down(p, data + (n - nb), (size_t)nb, 0);
        p->dedup_delivered = now;
        g_fec_dedup_bytes += (uint64_t)(n - nb);
    } else if(n){
        g_fec_dedup_bytes += (uint64_t)n;
    }
    if(fin && !p->down_fin_sent){ p->down_fin_sent=1; fwd_down(p, NULL, 0, 1); }
}

/* exit FEC 去重交付: 双发两条上游 stream(up/up2)承载完全相同的有序字节流。对每条维护累计接收字节,
 * 只把"累计已超过 dedup_delivered"的新增部分交付给 target(q2t): 领先的先交付、落后的追上不重复。
 * 单发(仅 up, flowid=0)时 up_rcvd 单调等于 dedup_delivered, 等价于直接交付。fin 仅推进结束标记。 */
static void exit_deliver_dedup(proxy_stream_t* p, int from_up2, uint8_t* data, size_t n, int fin){
    ps_touch(p);
    uint64_t* rcvd = from_up2 ? &p->up2_rcvd : &p->up_rcvd;
    uint64_t now = *rcvd + n;
    *rcvd = now;
    if(now > p->dedup_delivered){
        uint64_t nb = now - p->dedup_delivered;      /* 本条新增可交付字节数 */
        if(nb > n) nb = n;                            /* 保险: 不超过本次收到量 */
        if(q2t_append(p, data + (n - nb), (size_t)nb)!=0){ p->need_teardown=1; return; }
        p->dedup_delivered = now;
        g_fec_dedup_bytes += (uint64_t)(n - nb);
    } else if(n){
        g_fec_dedup_bytes += (uint64_t)n;
    }
    if(fin) p->q2t_fin=1;   /* 两条等长, 任一条 fin 即数据完整, 交付排空后半关 target */
}

/* middle/exit: 收上一跳 stream 数据(去程)。首解析 route 首部, 再转发。
 * from_up2: 本批数据来自 FEC 第二条上游 stream(exit 去重按来源分别累计)。 */
static int on_up_data(proxy_stream_t* p, uint8_t* bytes, size_t len, int fin, int from_up2){
    size_t off=0;
    if(!p->hdr_done){
        while(off<len && p->hdr_len<sizeof(p->hdr)-1){
            char ch=(char)bytes[off++]; if(ch=='\n'){ p->hdr[p->hdr_len]=0; p->hdr_done=1; break; }
            p->hdr[p->hdr_len++]=ch;
        }
        if(!p->hdr_done) return 0; /* 继续等首部 */
        snprintf(p->route,sizeof(p->route),"%s",p->hdr);
        /* 解析首部前缀 <prio>;<flowid>; (端到端优先级 + 本跳双发组 ID)。
         * 向后兼容旧 <prio>; 两段格式: 第二段非纯数字则视为 route, flowid=0(单发)。 */
        int prio=PRIO_BULK; uint32_t flowid=0; char* rt=p->hdr;
        { char* semi=strchr(rt,';');
          if(semi && semi>rt){ int ok=1; for(char* q=rt;q<semi;q++) if(*q<'0'||*q>'9'){ok=0;break;}
            if(ok){ prio=atoi(rt); rt=semi+1;
                char* semi2=strchr(rt,';');
                if(semi2 && semi2>rt){ int ok2=1; for(char* q=rt;q<semi2;q++) if(*q<'0'||*q>'9'){ok2=0;break;}
                    if(ok2){ flowid=(uint32_t)strtoul(rt,NULL,10); rt=semi2+1; } } } } }
        p->prio=prio;
        picoquic_set_stream_priority(p->up_cnx,p->up_stream_id,(uint8_t)prio);
        /* FEC 配对(exit/middle): 非0 flowid 且已有同 flowid 的 first ps -> 本条(p 的 up)是双发第二条:
         * 挂为 first 的 up2, 首部后数据去重交付给 first, 释放临时 p(不 discard stream, 归 first)。
         * 单线程串行保证第一条已先设好 flowid, 第二条必能配对到; 谁先到谁 first, 与去重无关。 */
        if((G.role==ROLE_EXIT || G.role==ROLE_MIDDLE) && flowid!=0){
            proxy_stream_t* first=ps_find_by_flowid(flowid);   /* p->flowid 尚未设, 不会匹配自身 */
            if(first){
                first->up2_cnx=p->up_cnx; first->up2_stream_id=p->up_stream_id;
                picoquic_set_app_stream_ctx(p->up_cnx,p->up_stream_id,first);
                log4c_debug("%s id=%u FEC pair flowid=%u up2_sid=%llu -> first id=%u",
                    role_name(G.role),p->id,flowid,(unsigned long long)p->up_stream_id,first->id);
                if(off<len){
                    if(G.role==ROLE_EXIT) exit_deliver_dedup(first,1,bytes+off,len-off,0);
                    else middle_deliver_dedup(first,1,bytes+off,len-off,0);
                }
                if(fin){
                    first->up_fin_seen=1;
                    if(G.role==ROLE_EXIT) exit_deliver_dedup(first,1,NULL,0,1);
                    else middle_deliver_dedup(first,1,NULL,0,1);
                }
                p->up_cnx=NULL; p->up_stream_id=0;   /* 解除临时 p 对该 stream 的所有权, 防误匹配 */
                ps_free(p);
                return 0;
            }
        }
        p->flowid=flowid;   /* 第一条(或单发): 记 flowid, 继续正常建连 */
        /* 首部(去 prio 前缀后)第一段(到第一个 ',') = 本节点动作; 其余 = 转发给下一跳 */
        char first[300], rest[300]; rest[0]=0;
        char* comma=strchr(rt,',');
        if(comma){ size_t fl=(size_t)(comma-rt); if(fl>=sizeof(first))fl=sizeof(first)-1;
            memcpy(first,rt,fl); first[fl]=0; snprintf(rest,sizeof(rest),"%s",comma+1); }
        else { snprintf(first,sizeof(first),"%s",rt); }

        if(strncmp(first,"T:",2)==0){
            /* exit: 异步解析 target 域名(不阻塞事件循环), 解析完成后在主循环发起非阻塞 connect */
            char thost[256]; int tport=0; char* c=strrchr(first+2,':');
            if(c){ *c=0; snprintf(thost,sizeof(thost),"%s",first+2); tport=atoi(c+1); }
            if(!whitelist_allowed(thost,tport)){ /* 白名单外 -> 拒绝(reset stream), 出口访问控制 */
                log4c_debug("exit id=%u BLOCKED %s:%d (not in whitelist)",p->id,thost,tport);
                ps_teardown(p); return -1; }
            if(dns_submit(p->id,thost,tport)!=0){ log4c_error("id=%u dns queue full %s:%d",p->id,thost,tport);
                ps_teardown(p); return -1; }
            p->dns_pending=1; p->target_port=tport;
            log4c_debug("exit id=%u sid=%llu -> resolving %s:%d (async) prio=%d",p->id,(unsigned long long)p->up_stream_id,thost,tport,prio);
        } else if(strncmp(first,"H:",2)==0){
            /* middle: 动态连下一跳 QUIC(地址来自首部), 转发剩余 route(带同一 prio)。 */
            char nhost[256]; int nport=0; char* c=strrchr(first+2,':');
            if(c){ *c=0; snprintf(nhost,sizeof(nhost),"%s",first+2); nport=atoi(c+1); }
            struct sockaddr_storage naddr; int is_name=0;
            if(picoquic_get_server_address(nhost,nport,&naddr,&is_name)!=0){
                log4c_error("id=%u resolve nexthop %s:%d fail",p->id,nhost,nport);
                ps_teardown(p); return -1; }
            if(!G.pool.configured){ if(pool_init(&G.pool,&naddr)!=0){ ps_teardown(p); return -1; } }
            if(downstream_open_stream(p, rest[0]?rest:"", prio)!=0){ ps_teardown(p); return -1; }
            log4c_debug("middle id=%u up_sid=%llu -> nexthop %s:%d down_sid=%llu rest=[%s]",
                p->id,(unsigned long long)p->up_stream_id,nhost,nport,(unsigned long long)p->down_stream_id,rest);
        } else {
            log4c_error("id=%u bad route first-seg=[%s]",p->id,first);
            ps_teardown(p); return -1;
        }
    }
    /* 首部之后的数据 -> 转发 right(exit 走 FEC 去重交付, 兼容单发; middle 直接转下游) */
    if(off<len){
        if(G.role==ROLE_EXIT) exit_deliver_dedup(p,from_up2,bytes+off,len-off,0);
        else if(G.role==ROLE_MIDDLE && p->flowid!=0) middle_deliver_dedup(p,from_up2,bytes+off,len-off,0);
        else fwd_down(p,bytes+off,len-off,0);
    }
    if(fin){ p->up_fin_seen=1;
        if(G.role==ROLE_EXIT) exit_deliver_dedup(p,from_up2,NULL,0,1);
        else if(G.role==ROLE_MIDDLE && p->flowid!=0) middle_deliver_dedup(p,from_up2,NULL,0,1);
        else fwd_down(p,NULL,0,1);
    }
    return 0;
}

/* QUIC 回调: 统一处理 up(去程) / down(回程) 两侧 stream 事件 */
static int relay_quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t ev, void* cb_ctx, void* v_stream_ctx){
    (void)cb_ctx;(void)v_stream_ctx;
    proxy_stream_t* p;
    switch(ev){
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin: {
        int fin=(ev==picoquic_callback_stream_fin);
        /* 先按下游(回程)匹配 */
        p=ps_find_by_down(cnx,stream_id);
        if(p!=NULL){
            if(length>0) fwd_up(p,bytes,length,0);
            if(fin){ p->down_fin_seen=1; if(G.role==ROLE_ENTRY) p->q2t_fin=1; else fwd_up(p,NULL,0,1); }
            break;
        }
        /* 再按上游(去程)匹配 */
        p=ps_find_by_up(cnx,stream_id);
        if(p==NULL){
            /* middle/exit: 新上游 stream(server 端被动收) */
            if(G.role==ROLE_ENTRY){ break; } /* entry 无 server, 忽略 */
            p=ps_alloc(); if(!p){ log4c_warn("stream table full, reset sid=%llu",(unsigned long long)stream_id);
                picoquic_discard_stream(cnx,stream_id,0); break; }
            p->up_cnx=cnx; p->up_stream_id=stream_id;
            picoquic_set_app_stream_ctx(cnx,stream_id,p);
        }
        int from_up2=(p->up2_cnx==cnx && p->up2_stream_id==stream_id);
        on_up_data(p,bytes,length,fin,from_up2);
        break; }
    case picoquic_callback_prepare_to_send:
        break; /* 用 add_to_stream, 不用主动发送 API */
    case picoquic_callback_stream_reset:
    case picoquic_callback_stop_sending:
        /* 对端 reset/stop 单条 stream -> 主动 teardown, discard 另一侧 stream 连锁通知(middle 据此回收) */
        if((p=ps_find_by_down(cnx,stream_id))!=NULL){ ps_teardown(p); }
        else if((p=ps_find_by_up(cnx,stream_id))!=NULL){ ps_teardown(p); }
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset: {
        int i; for(i=0;i<MAX_CONN;i++) if(G.streams[i].in_use && (G.streams[i].up_cnx==cnx||G.streams[i].down_cnx==cnx||G.streams[i].up2_cnx==cnx||G.streams[i].down2_cnx==cnx)) ps_free(&G.streams[i]);
        for(i=0;i<POOL_SIZE;i++) if(G.pool.cnx[i]==cnx){
            G.pool.cnx[i]=NULL; G.pool.recent_loss[i]=0; G.pool.recent_rtt[i]=0; G.pool.recent_rtt_max[i]=0;
            G.pool.recent_sent[i]=0; G.pool.last_sent_total[i]=0; G.pool.last_lost_total[i]=0;
            G.pool.last_spurious_total[i]=0; G.pool.recent_rtt_var[i]=0; G.pool.recent_ts[i]=0;
        } /* 从池移除, 下次 pool_pick 惰性重建 */
        picoquic_set_callback(cnx,NULL,NULL); break; }
    default: break;
    }
    return 0;
}

/* entry: 建到第一跳的 QUIC 连接池(POOL_SIZE 条并行连接) */
static int entry_open_upstream(void){
    return pool_init(&G.pool, &G.next_addr);
}

/* entry: 接受一个 TCP 客户端连接 */
static void entry_accept(void){
    int fd=accept(G.tcp_listen_fd,NULL,NULL);
    if(fd<0) return;
    set_nonblock(fd); int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    proxy_stream_t* p=ps_alloc();
    if(!p){ log4c_warn("stream table full, drop tcp accept"); close(fd); return; }
    p->tcp_fd=fd;
    if(G.socks_enabled){
        p->socks_stage=0; /* 等 SOCKS5 握手拿到 target 后再开 QUIC stream */
        log4c_debug("entry id=%u accept fd=%d (socks, awaiting handshake)",p->id,fd);
        return;
    }
    snprintf(p->route,sizeof(p->route),"%s",G.route_str);
    { int prio = classify_target(G.route_str);
      log4c_info("entry id=%u classify fixed route=%s prio=%d", p->id, G.route_str, prio);
      if(downstream_open_stream(p,G.route_str,prio)!=0){ ps_free(p); return; } }
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
        uint8_t rep[2]={0x05,0x00}; /* 选无认证 */
        (void)send(p->tcp_fd,rep,2,MSG_NOSIGNAL);
        size_t used=2+nm; memmove(p->socks_buf,p->socks_buf+used,p->socks_len-used); p->socks_len-=used;
        p->socks_stage=1;
    }
    if(p->socks_stage==1){ /* request: VER CMD RSV ATYP ADDR PORT */
        if(p->socks_len<4) return;
        uint8_t cmd=p->socks_buf[1], atyp=p->socks_buf[3];
        char host[256]; int port=0; size_t need=0;
        if(atyp==0x01){ need=4+4+2; if(p->socks_len<need) return;
            snprintf(host,sizeof(host),"%u.%u.%u.%u",p->socks_buf[4],p->socks_buf[5],p->socks_buf[6],p->socks_buf[7]);
            port=(p->socks_buf[8]<<8)|p->socks_buf[9]; }
        else if(atyp==0x03){ uint8_t dl=p->socks_buf[4]; need=4+1+(size_t)dl+2; if(p->socks_len<need) return;
            memcpy(host,p->socks_buf+5,dl); host[dl]=0; port=(p->socks_buf[5+dl]<<8)|p->socks_buf[6+dl]; }
        else if(atyp==0x04){ need=4+16+2; if(p->socks_len<need) return;
            inet_ntop(AF_INET6,p->socks_buf+4,host,sizeof(host)); port=(p->socks_buf[20]<<8)|p->socks_buf[21]; }
        else { uint8_t r[10]={0x05,0x08,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL); ps_free(p); return; }
        if(cmd!=0x01){ /* 仅支持 CONNECT */
            uint8_t r[10]={0x05,0x07,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL); ps_free(p); return; }
        if(!whitelist_allowed(host,port)){ /* 白名单外 -> 拒绝(0x02 not allowed by ruleset), 不占三跳线路 */
            log4c_debug("entry id=%u BLOCKED %s:%d (not in whitelist)",p->id,host,port);
            uint8_t r[10]={0x05,0x02,0,0x01,0,0,0,0,0,0}; (void)send(p->tcp_fd,r,10,MSG_NOSIGNAL); ps_free(p); return; }
        uint8_t rep[10]={0x05,0x00,0x00,0x01,0,0,0,0,0,0}; /* 成功, BND 全 0 */
        (void)send(p->tcp_fd,rep,10,MSG_NOSIGNAL);
        memmove(p->socks_buf,p->socks_buf+need,p->socks_len-need); p->socks_len-=need;
        /* 构造 route: 中间跳前缀(可空) + 动态 target */
        char route[300];
        if(G.mid_route[0]) snprintf(route,sizeof(route),"%s,T:%s:%d",G.mid_route,host,port);
        else snprintf(route,sizeof(route),"T:%s:%d",host,port);
        snprintf(p->route,sizeof(p->route),"%s",route);
        int prio=classify_target(host); /* 直播/实时 -> 低延迟专用道 */
        log4c_info("entry id=%u classify host=%s port=%d prio=%d route=%s", p->id, host, port, prio, route);
        if(downstream_open_stream(p,route,prio)!=0){ ps_free(p); return; }
        p->socks_stage=2;
        log4c_debug("entry id=%u socks CONNECT %s:%d -> down_sid=%llu route=%s",p->id,host,port,(unsigned long long)p->down_stream_id,route);
        /* request 之后可能已跟随应用数据 -> 转发到下游 */
        if(p->socks_len>0){ fwd_down(p,p->socks_buf,p->socks_len,0); p->socks_len=0; }
    }
}

/* 把面向 TCP 的待写缓冲(q2t) flush 到 TCP fd */
static void flush_q2t(proxy_stream_t* p){
    if(p->tcp_fd<0) return;
    while(p->q2t_len>0){
        ssize_t n=send(p->tcp_fd,p->q2t,p->q2t_len,MSG_NOSIGNAL);
        if(n>0){ memmove(p->q2t,p->q2t+n,p->q2t_len-(size_t)n); p->q2t_len-=(size_t)n; ps_touch(p); }
        else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK){
            /* 本地 TCP 已死(EPIPE/ECONNRESET/...): 待写数据无处可去, 丢弃并标记 EOF -> 触发回收,
             * 否则 q2t_len 恒>0 使 maybe_free 永不满足, 流泄漏。 */
            log4c_debug("id=%u q2t send err=%s, drop %zu & teardown",p->id,strerror(errno),p->q2t_len);
            p->tcp_eof=1; p->q2t_len=0;
            break;
        }
        else break; /* EAGAIN: 等下轮可写 */
    }
    if(p->q2t_len==0 && p->q2t_fin){ shutdown(p->tcp_fd,SHUT_WR); p->q2t_fin=0; }
}

/* 从 TCP fd 读 -> add_to_stream 到 QUIC。entry: 写下游; exit: 写上游(回程)。 */
static void pump_tcp(proxy_stream_t* p){
    uint8_t buf[16384]; ssize_t n;
    for(;;){
        n=recv(p->tcp_fd,buf,sizeof(buf),0);
        if(n>0){ if(G.role==ROLE_ENTRY) fwd_down(p,buf,(size_t)n,0); else fwd_up(p,buf,(size_t)n,0); }
        else if(n==0){ p->tcp_eof=1;
            if(G.role==ROLE_ENTRY) fwd_down(p,NULL,0,1); else fwd_up(p,NULL,0,1);
            break; }
        else break; /* EAGAIN */
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
    if(left_done && right_done && p->q2t_len==0){ ps_teardown(p); return; }
    if(G.role==ROLE_ENTRY && p->tcp_eof && p->q2t_len==0){ ps_teardown(p); return; }
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
        "  entry(固定target): %s -r entry -l <tcp_port> -n <hop1_host> -N <hop1_qport> -R <route>\n"
        "  entry(SOCKS5入口): %s -r entry -l <socks_port> -n <hop1_host> -N <hop1_qport> -S [-M <mid_route>]\n"
        "  middle           : %s -r middle -p <quic_port> -c <cert> -k <key>\n"
        "  exit             : %s -r exit   -p <quic_port> -c <cert> -k <key>\n"
        "  route(发给第一跳): \"H:hop2:qport,...,T:target:port\" (两跳仅 \"T:target:port\")\n"
        "  -S: SOCKS5 入口, target 由 SOCKS5 CONNECT 动态获取; -M: 中间跳前缀如 \"H:kz:4443\"(空=两跳)\n",
        prog,prog,prog,prog);
}

int main(int argc,char**argv){
    int i; int listen_port=0, quic_port=0, next_port=0;
    const char* rolestr=NULL; const char* next_host=NULL;
    const char* cert=NULL; const char* key=NULL; const char* route=NULL;
    const char* wl_path=NULL;
    memset(&G,0,sizeof(G)); G.tcp_listen_fd=-1; G.udp_fd=-1;

    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"-r")&&i+1<argc) rolestr=argv[++i];
        else if(!strcmp(argv[i],"-l")&&i+1<argc) listen_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-p")&&i+1<argc) quic_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-n")&&i+1<argc) next_host=argv[++i];
        else if(!strcmp(argv[i],"-N")&&i+1<argc) next_port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-R")&&i+1<argc) route=argv[++i];
        else if(!strcmp(argv[i],"-S")) G.socks_enabled=1;
        else if(!strcmp(argv[i],"-W")&&i+1<argc) wl_path=argv[++i];
        else if(!strcmp(argv[i],"-M")&&i+1<argc) snprintf(G.mid_route,sizeof(G.mid_route),"%s",argv[++i]);
        else if(!strcmp(argv[i],"-c")&&i+1<argc) cert=argv[++i];
        else if(!strcmp(argv[i],"-k")&&i+1<argc) key=argv[++i];
    }
    if(rolestr==NULL){ usage(argv[0]); return 1; }
    if(!strcmp(rolestr,"entry")) G.role=ROLE_ENTRY;
    else if(!strcmp(rolestr,"middle")) G.role=ROLE_MIDDLE;
    else if(!strcmp(rolestr,"exit")) G.role=ROLE_EXIT;
    else { usage(argv[0]); return 1; }

    { char pname[32]; snprintf(pname,sizeof(pname),"nb-%s",role_name(G.role)); log4c_init(pname);
      /* 生产默认 INFO(高频 per-stream 日志已降为 DEBUG, 不输出), 避免海量流日志拖慢单线程事件循环。
       * 需调试时 NB_LOG_LEVEL=DEBUG。 */
      const char* lv=getenv("NB_LOG_LEVEL"); int lvl=LOG4C_INFO;
      if(lv){ if(!strcmp(lv,"DEBUG"))lvl=LOG4C_DEBUG; else if(!strcmp(lv,"WARN"))lvl=LOG4C_WARN; else if(!strcmp(lv,"ERROR"))lvl=LOG4C_ERROR; }
      log4c_set_level(lvl); }

    { const char* fe=getenv("NB_FEC");    /* FEC 双发开关: 仅 middle 对直播流(瓶颈跳 middle->exit)双发; 默认关 */
      if(fe && (!strcmp(fe,"on")||!strcmp(fe,"1"))){ g_fec_enabled=1; log4c_info("FEC dual-send: ENABLED (NB_FEC=%s)",fe); } }

    wl_init(wl_path); /* 白名单(仅 entry -W 指定时启用; 未指定=全放行) */

    if(G.role==ROLE_ENTRY){
        int is_name=0;
        if(!listen_port||!next_host||!next_port||(!G.socks_enabled && !route)){ usage(argv[0]); log4c_shutdown(); return 1; }
        if(picoquic_get_server_address(next_host,next_port,&G.next_addr,&is_name)!=0){
            log4c_error("resolve hop1 %s:%d fail",next_host,next_port); log4c_shutdown(); return 1; }
        G.next_configured=1;
        if(!G.socks_enabled) snprintf(G.route_str,sizeof(G.route_str),"%s",route);
    } else {
        if(!quic_port||!cert||!key){ usage(argv[0]); log4c_shutdown(); return 1; }
    }

    /* QUIC context: entry 纯 client(cert=NULL); middle/exit 是 server(cert/key) 且 middle 兼作 client。 */
    G.quic=picoquic_create(MAX_CONN, G.role==ROLE_ENTRY?NULL:cert, G.role==ROLE_ENTRY?NULL:key, NULL, NB_ALPN,
        G.role==ROLE_ENTRY?NULL:relay_quic_callback, G.role==ROLE_ENTRY?NULL:&G, NULL,NULL,NULL,
        picoquic_current_time(), NULL,NULL,NULL,0);
    if(G.quic==NULL){ log4c_error("quic create fail"); log4c_shutdown(); return 1; }
    if(G.role!=ROLE_ENTRY) picoquic_set_cookie_mode(G.quic,2);
    picoquic_set_default_congestion_algorithm(G.quic, picoquic_bbr_algorithm); /* BBRv3(bbr.c) */
    /* 放开传输参数上限。单流吞吐 ≈ max_stream_data / RTT: 三跳 gz→hk→kz RTT~300ms 下,
     * 旧 1MiB 窗口把单流限死在 ~26Mbps。放到 8MiB -> 单流理论上限 ~210Mbps(覆盖 80Mbps 峰值)。
     * connection 级 max_data 64MiB 支撑大量并发流叠加吞吐, 并缓解多流共享连接窗口的背压。 */
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_streams_bidi, 2000);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_stream_data_bidi_local, 8388608);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_stream_data_bidi_remote, 8388608);
    picoquic_set_default_tp_value(G.quic, picoquic_tp_initial_max_data, 67108864);
    /* 优化2: 降低 max_ack_delay(默认25ms)到 5ms -> 接收方更快回 ACK, 拥塞控制更灵敏、RTT 感知更低,
     * 对直播/实时流延迟直接有益(代价: ACK 包略增, 80Mbps 下开销可忽略)。 */
    picoquic_set_default_tp_value(G.quic, picoquic_tp_max_ack_delay, 5000);

    /* UDP socket: entry 用临时端口(client); middle/exit bind quic_port(server)。 */
    G.udp_fd=socket(AF_INET,SOCK_DGRAM,0);
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
        struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons((uint16_t)listen_port);
        if(bind(G.tcp_listen_fd,(struct sockaddr*)&a,sizeof(a))<0){ log4c_error("bind tcp :%d fail: %s",listen_port,strerror(errno)); log4c_shutdown(); return 1; }
        listen(G.tcp_listen_fd,128); set_nonblock(G.tcp_listen_fd);
        if(entry_open_upstream()!=0){ log4c_error("entry open upstream fail"); log4c_shutdown(); return 1; }
        if(G.socks_enabled)
            log4c_info("entry up: SOCKS5 :%d -> QUIC %s:%d mid=[%s] (BBRv2)",listen_port,next_host,next_port,G.mid_route);
        else
            log4c_info("entry up: TCP :%d -> QUIC %s:%d route=%s (BBRv2)",listen_port,next_host,next_port,route);
    } else {
        log4c_info("%s up: QUIC :%d (BBRv2)",role_name(G.role),quic_port);
    }

    dns_init(); /* 异步 DNS 线程池 + self-pipe(主要 exit 用; 其他角色 worker idle 无害) */

    /* ---- 自定义事件循环 ---- */
    for(;;){
        fd_set rfds,wfds; FD_ZERO(&rfds); FD_ZERO(&wfds);
        int maxfd=G.udp_fd; FD_SET(G.udp_fd,&rfds);
        FD_SET(DNS.pipe_rd,&rfds); if(DNS.pipe_rd>maxfd)maxfd=DNS.pipe_rd;
        if(G.tcp_listen_fd>=0){ FD_SET(G.tcp_listen_fd,&rfds); if(G.tcp_listen_fd>maxfd)maxfd=G.tcp_listen_fd; }
        for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i]; if(!p->in_use||p->tcp_fd<0)continue;
            FD_SET(p->tcp_fd,&rfds); if(p->q2t_len>0||p->tcp_connecting) FD_SET(p->tcp_fd,&wfds);
            if(p->tcp_fd>maxfd)maxfd=p->tcp_fd; }
        uint64_t now=picoquic_current_time();
        int64_t wd=picoquic_get_next_wake_delay(G.quic,now,1000000); /* us, cap 1s */
        struct timeval tv; tv.tv_sec=wd/1000000; tv.tv_usec=wd%1000000;
        (void)select(maxfd+1,&rfds,&wfds,NULL,&tv);
        now=picoquic_current_time();
        /* 1) UDP -> QUIC (recvmmsg 批量接收, 降 syscall) */
        if(FD_ISSET(G.udp_fd,&rfds)) udp_drain(now);
        /* 1.5) 异步 DNS 完成 -> 主线程发起非阻塞 connect target(exit)。不再在事件循环内阻塞解析。 */
        if(FD_ISSET(DNS.pipe_rd,&rfds)){
            char drain[256]; while(read(DNS.pipe_rd,drain,sizeof(drain))>0){}
            dns_res_t r;
            while(dns_pop_result(&r)){
                proxy_stream_t* p=ps_find_by_id(r.ps_id);
                if(!p||!p->dns_pending) continue;           /* 流已释放/状态变更 -> 丢弃结果 */
                p->dns_pending=0;
                if(!r.ok){ log4c_warn("id=%u dns resolve fail",p->id); ps_teardown(p); continue; }
                int fd=tcp_connect_addr((struct sockaddr*)&r.addr,r.addrlen);
                if(fd<0){ log4c_warn("id=%u connect after dns fail: %s",p->id,strerror(errno)); ps_teardown(p); continue; }
                p->tcp_fd=fd; p->tcp_connecting=1;
                log4c_debug("exit id=%u sid=%llu -> target connected fd=%d port=%d",p->id,(unsigned long long)p->up_stream_id,fd,r.port);
            }
        }
        /* 2) TCP accept(entry) */
        if(G.tcp_listen_fd>=0 && FD_ISSET(G.tcp_listen_fd,&rfds)) entry_accept();
        /* 3) 每条流的 TCP IO */
        for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i]; if(!p->in_use||p->tcp_fd<0)continue;
            if(p->tcp_connecting && FD_ISSET(p->tcp_fd,&wfds)){ int se=0; socklen_t sl=sizeof(se);
                getsockopt(p->tcp_fd,SOL_SOCKET,SO_ERROR,&se,&sl); p->tcp_connecting=0;
                if(se!=0){ log4c_warn("id=%u target connect fail: %s",p->id,strerror(se));
                    picoquic_reset_stream(p->up_cnx,p->up_stream_id,1); ps_free(p); continue; } }
            if(FD_ISSET(p->tcp_fd,&rfds)){
                if(G.socks_enabled && p->socks_stage<2){ socks_handshake(p); if(!p->in_use) continue; }
                else pump_tcp(p);
            }
            flush_q2t(p);
            maybe_free(p);
        }
        /* middle: 无 TCP 的流也要判释放 */
        if(G.role==ROLE_MIDDLE){ for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i]; if(p->in_use&&p->tcp_fd<0) maybe_free(p); } }
        /* 过载保护: q2t 超限标记的流, 主循环兜底 teardown(不在回调栈内拆, 避免重入/use-after-free) */
        for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i]; if(p->in_use && p->need_teardown) ps_teardown(p); }
        /* 空闲超时兜底: 无数据活动超 IDLE_TIMEOUT 的流强制拆除(防僵尸流累积拖死复用连接; 半开/对端不关 FIN 亦兜住) */
        { uint64_t tnow=picoquic_current_time();
          for(i=0;i<MAX_CONN;i++){ proxy_stream_t*p=&G.streams[i];
            if(p->in_use && tnow > p->last_active && tnow - p->last_active > IDLE_TIMEOUT_US){
                log4c_warn("id=%u idle timeout(%llus), teardown route=%s",p->id,
                    (unsigned long long)((tnow-p->last_active)/1000000),p->route);
                ps_teardown(p); } } }
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
        if(G.pool.configured){ static uint64_t lq_last=0; uint64_t tn=picoquic_current_time();
            if(tn - lq_last > 10000000ULL){ lq_last=tn;
                for(int k=0;k<POOL_SIZE;k++){ if(!G.pool.cnx[k]) continue;
                    picoquic_path_quality_t q; memset(&q,0,sizeof(q));
                    picoquic_get_default_path_quality(G.pool.cnx[k], &q);
                    uint64_t delta_sent = (q.sent >= G.pool.last_sent_total[k]) ? (q.sent - G.pool.last_sent_total[k]) : q.sent;
                    uint64_t delta_lost = (q.lost >= G.pool.last_lost_total[k]) ? (q.lost - G.pool.last_lost_total[k]) : q.lost;
                    uint64_t delta_spurious = (q.spurious_losses >= G.pool.last_spurious_total[k])
                        ? (q.spurious_losses - G.pool.last_spurious_total[k]) : q.spurious_losses;
                    uint64_t eff_lost = (delta_lost > delta_spurious) ? (delta_lost - delta_spurious) : 0;
                    double loss = delta_sent ? (100.0*(double)eff_lost/(double)delta_sent) : 0.0;
                    G.pool.recent_loss[k] = loss;
                    G.pool.recent_rtt[k] = q.rtt;
                    G.pool.recent_rtt_max[k] = q.rtt_max;
                    G.pool.recent_sent[k] = delta_sent;
                    G.pool.last_sent_total[k] = q.sent;
                    G.pool.last_lost_total[k] = q.lost;
                    G.pool.last_spurious_total[k] = q.spurious_losses;
                    G.pool.recent_rtt_var[k] = q.rtt_variant;
                    G.pool.recent_ts[k] = tn;
                    log4c_info("linkq pool[%d] rtt=%.1fms(min%.1f/max%.1f) loss=%.2f%% lost_pkt=%llu cwin=%lluKB bw=%lluKbps sent=%lluKB",
                        k, q.rtt/1000.0, q.rtt_min/1000.0, q.rtt_max/1000.0, loss,
                        (unsigned long long)q.lost,
                        (unsigned long long)(q.cwin/1024),
                        (unsigned long long)(q.pacing_rate*8/1000),
                        (unsigned long long)(q.bytes_sent/1024));
                }
            }
        }
        /* 4) QUIC -> UDP (GSO 批量发送, 降 syscall; 不支持时逐段回退) */
        /* FEC 观测(每10s): 双发发起数/退化单发数 + 去重省下的重复字节, 看双发是否真生效。 */
        { static uint64_t fecstat_last=0; uint64_t ftn=picoquic_current_time();
          if(ftn - fecstat_last > 10000000ULL){ fecstat_last=ftn;
            if(g_fec_flow_started||g_fec_fallback_single||g_fec_dedup_bytes)
              log4c_info("fec stat: flow_started=%llu fallback_single=%llu dedup=%lluKB",
                (unsigned long long)g_fec_flow_started,(unsigned long long)g_fec_fallback_single,
                (unsigned long long)(g_fec_dedup_bytes/1024)); } }
        udp_send_batch(now);
    }
    log4c_shutdown();
    return 0;
}
