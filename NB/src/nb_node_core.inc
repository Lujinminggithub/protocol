/* NB entry/middle/exit unified node. Source route format:
 * H:next-hop:port,...,T:target:port. The epoll loop drives picoquic and
 * bridges the client side to the target side through prepare_to_send. */
#include <stdint.h>
#include <limits.h>
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
#if defined(__GLIBC__)
#include <malloc.h>
#endif
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
#include "nb_fec_policy.h"
#include "nb_ring.h"
#include "nb_auth.h"
#include "nb_auth_async.h"
#include "nb_tenant.h"
#include "nb_control.h"
#include "nb_routes.h"
#include "nb_policy.h"
#include "nb_live.h"
#include "nb_probe.h"
#include "nb_lstream.h"
#include "nb_metrics.h"
#include "nb_session_index.h"
#include "nb_session.h"
#include "nb_runtime.h"
#include "nb_instance.h"
#include "nb_bridge.h"
#include "nb_send.h"
#include "nb_path.h"
#include "nb_dns.h"
#include "nb_udp.h"
#include "nb_udp_io.h"
#include "nb_udp_lifecycle.h"
#include "nb_whitelist.h"
#include "nb_shard.h"
#include "log/log4c.h"

#define NB_ALPN "nb/1"          /* Newbility(NB) 新传输协议 ALPN */
#define NB_SNI  "nb.internal"
#define NB_PROBE_PORT 9
#define NB_PROBE_SINK_HOST "nb-probe-sink.internal"
#define NB_PROBE_ECHO_HOST "nb-probe-echo.internal"
#define BUFCAP (256*1024)
#define MAX_CONN NB_MAX_CONNECTIONS
#define NB_Q2T_MAX (16*1024*1024)   /* 过载保护: 单流 q2t 待写缓冲硬上限 16MB, 超限=对端TCP卡死 */
#define NB_QTX_MAX (16*1024*1024)   /* 过载保护: 单流 QUIC 侧应用发送缓冲硬上限 16MB */
#define NB_TCP_TX_HIGH (2*1024*1024)
#define NB_TCP_TX_LOW  (1024*1024)
#define NB_FEC_DG_HIGH (192*1024)
#define NB_FEC_DG_LOW  (96*1024)
#define NB_FEC_DG_INFLIGHT_MAX 64u
#define NB_STREAM_FLOW_WINDOW (128*1024ULL)
#define NB_STREAM_FLOW_CREDIT_QUANTUM (32*1024ULL)
#define NB_DGRAM_QUEUE_MAX (256*1024) /* FEC sidecar datagram 应用发送队列上限 */
#define NB_EMPTY_QUEUE_RETAIN_MAX (256*1024)
#define NB_MEMORY_SCAN_INTERVAL_US (5*1000000ULL)
#define NB_MALLOC_TRIM_INTERVAL_US (30*1000000ULL)
#define NB_MALLOC_TRIM_MIN_RELEASED (1024*1024ULL)
#define NB_ROUTE_BOOTSTRAP_PRIO 0       /* latency 流仅在发送 route header 时临时提权 */
#define IDLE_TIMEOUT_US (120*1000000ULL)  /* 流空闲(无数据)超时兜底回收, 防僵尸流累积拖死复用连接 */
#define SOCKS_AUTH_CACHE_TTL_US (300*1000000ULL)
#define POOL_SIZE NB_POOL_SIZE              /* A/B 稳定基线：每跳仅一个 QUIC 拥塞控制实例 */
#define NB_MAX_POOLS NB_MAX_PATH_POOLS
#define BULK_POOL_START 3                  /* 批量流优先使用后半池；实时流可选择全池 */
#define NB_FEC_CTRL_PREFIX "FC:"
#define NB_FEC_CTRL_START  "FC:START"
#define NB_FEC_CTRL_ACK    "FC:ACK"
#define NB_FEC_CTRL_RX_CAP 4096u
#define NB_FEC_DGRAM_MAGIC 0x4E424644u /* 'NBFD' */
#define NB_FEC_DGRAM_VER   1
#define NB_FEC_DEFAULT_K 8
#define NB_FEC_DEFAULT_R 1

typedef enum {
    nb_fec_dgram_hello = 1,
    nb_fec_dgram_ack = 2,
    nb_fec_dgram_source = 3,
    nb_fec_dgram_repair = 4
} nb_fec_dgram_type_t;
/* 每条被代理的流: left 侧 <-> right 侧, 双向缓冲/配对。 */

/* 到下一跳的 QUIC 连接池: 每条连接独立 congestion window / pacing / flow-control,
 * 新 stream round-robin 分散到各连接, 使总吞吐叠加而非稀释, 且单连接故障/僵死不影响全局。 */


static _Thread_local nb_instance_t* nb_current_instance;
#define G (nb_current_instance->global)
#define g_run_id (nb_current_instance->run_id)
#define g_socks_users (nb_current_instance->socks_users)
#define g_socks_auth_enabled (nb_current_instance->socks_auth_enabled)
#define g_tenants (nb_current_instance->tenants)
#define g_tenants_enabled (nb_current_instance->tenants_enabled)
#define g_socks_udp_advertise_addr (nb_current_instance->socks_udp_advertise_addr)
#define g_socks_udp_advertise_configured (nb_current_instance->socks_udp_advertise_configured)
#define g_socks_udp_port_min (nb_current_instance->socks_udp_port_min)
#define g_socks_udp_port_max (nb_current_instance->socks_udp_port_max)
#define g_socks_udp_port_next (nb_current_instance->socks_udp_port_next)
#define g_ps_inuse (nb_current_instance->streams_inuse)
#define g_ps_peak (nb_current_instance->streams_peak)
#define g_udp_gso_enabled (nb_current_instance->udp_gso_enabled)
#define g_reorder_gap (nb_current_instance->reorder_gap)
#define g_reorder_delay_us (nb_current_instance->reorder_delay_us)
#define g_pool_health_cfg (nb_current_instance->pool_health_config)
#define g_fec_v15_observe (nb_current_instance->fec_v15_observe)
#define g_fec_v15_active (nb_current_instance->fec_v15_active)
#define g_fec_v15_force (nb_current_instance->fec_v15_force)
#define g_fec_v15_force_robust (nb_current_instance->fec_v15_force_robust)
#define g_fec_v15_drop_src_mod (nb_current_instance->fec_v15_drop_src_mod)
#define g_fec_v15_drop_repair_mod (nb_current_instance->fec_v15_drop_repair_mod)
#define g_fec_v15_drop_src (nb_current_instance->fec_v15_drop_src)
#define g_fec_v15_drop_repair (nb_current_instance->fec_v15_drop_repair)
#define g_fec_cfg (nb_current_instance->fec_config)
#define g_fec_bdp_bitrate_bps (nb_current_instance->fec_bdp_bitrate_bps)
#define g_fec_bdp_rtt_us (nb_current_instance->fec_bdp_rtt_us)
#define g_fec_bdp_multiplier (nb_current_instance->fec_bdp_multiplier)
#define g_fec_metrics_done (nb_current_instance->fec_metrics_done)
#define g_metrics_state (nb_current_instance->metrics_state)
#define g_udp_rxq_main (nb_current_instance->udp_rxq_main)
#define g_udp_rxq_client (nb_current_instance->udp_rxq_client)
#define g_session_index (nb_current_instance->session_index)
#define g_udp_lifecycle (nb_current_instance->udp_lifecycle)
#define g_udp_control_grace_us (nb_current_instance->udp_control_grace_us)
#define g_memory_released_pending (nb_current_instance->memory_released_pending)
#define g_fec_session_index (nb_current_instance->fec_session_index)
#define g_sched_cnx (nb_current_instance->sched_cnx)
static int worker_lane_port(int base){
    const char* enabled=nb_instance_env(nb_current_instance,"NB_WORKER_LANE_PORTS");
    if(!enabled||(!strcmp(enabled,"0")||!strcasecmp(enabled,"off")))return base;
    const char* text=nb_instance_env(nb_current_instance,"NB_WORKER_ID");char* end=NULL;long worker=text?strtol(text,&end,10):0;
    if((text&&(!end||*end))||worker<0||worker>31||base<1||base+worker>65535)return -1;
    return base+(int)worker;
}

static nb_live_sched_t* sched_for_cnx(picoquic_cnx_t* cnx,int create){
    if(cnx==NULL)return NULL;
    for(size_t i=0;i<NB_SCHED_CNX_CAP;i++)if(g_sched_cnx[i].cnx==cnx)return &g_sched_cnx[i].sched;
    if(!create)return NULL;
    for(size_t i=0;i<NB_SCHED_CNX_CAP;i++)if(g_sched_cnx[i].cnx==NULL){
        g_sched_cnx[i].cnx=cnx;memset(&g_sched_cnx[i].sched,0,sizeof(g_sched_cnx[i].sched));
        return &g_sched_cnx[i].sched;
    }
    return NULL;
}

static void udp_lifecycle_wake_due(uint64_t now_us){uintptr_t link;while((link=nb_udp_lifecycle_mark_due(&g_udp_lifecycle,now_us))!=0)(void)picoquic_mark_datagram_ready((picoquic_cnx_t*)link,1);}
static int udp_lifecycle_schedule(picoquic_cnx_t* cnx,uint8_t type,uint32_t session_id,const char* route){if(cnx==NULL||session_id==0||route==NULL||!*route)return -1;uint64_t now=picoquic_current_time();int rc=nb_udp_lifecycle_schedule(&g_udp_lifecycle,(uintptr_t)cnx,type,session_id,route,now);if(rc==0)udp_lifecycle_wake_due(now);return rc;}
static void udp_lifecycle_forget_cnx(picoquic_cnx_t* cnx){nb_udp_lifecycle_forget_link(&g_udp_lifecycle,(uintptr_t)cnx);}

static void sched_forget_cnx(picoquic_cnx_t* cnx){
    for(size_t i=0;i<NB_SCHED_CNX_CAP;i++)if(g_sched_cnx[i].cnx==cnx){memset(&g_sched_cnx[i],0,sizeof(g_sched_cnx[i]));return;}
}

static int sched_media_pending(picoquic_cnx_t* cnx,const proxy_stream_t* exclude){
    for(int i=0;i<MAX_CONN;i++){
        proxy_stream_t* q=&G.streams[i];if(!q->in_use||q==exclude||q->flow_class!=NB_FLOW_CLASS_MEDIA)continue;
        if((q->down_cnx==cnx&&q->down_tx.len>0)||(q->up_cnx==cnx&&q->up_tx.len>0))return 1;
    }
    return 0;
}

static size_t sched_stream_grant(proxy_stream_t* p,picoquic_cnx_t* cnx,size_t requested,uint64_t now){
    nb_live_sched_t* sched=sched_for_cnx(cnx,1);
    int media_pending=sched_media_pending(cnx,p);
    size_t granted=nb_live_sched_grant(sched,p->flow_class,requested,now,media_pending);
    if(p->flow_class==NB_FLOW_CLASS_CTRL&&media_pending&&granted<requested){
        p->sched_throttled=1;picoquic_set_stream_priority(cnx,
            p->down_cnx==cnx?p->down_stream_id:p->up_stream_id,NB_PRIO_MEDIA+2);
    }
    return granted;
}

static void sched_refresh_stream(proxy_stream_t* p,uint64_t now){
    if(p==NULL||!p->in_use||!p->sched_throttled||p->flow_class!=NB_FLOW_CLASS_CTRL)return;
    picoquic_cnx_t* connections[2]={p->down_cnx,p->up_cnx};
    uint64_t streams[2]={p->down_stream_id,p->up_stream_id};
    for(int i=0;i<2;i++)if(connections[i]&&nb_live_sched_ctrl_ready(sched_for_cnx(connections[i],1),now)){
        picoquic_set_stream_priority(connections[i],streams[i],NB_PRIO_CTRL);p->sched_throttled=0;
    }
}
#define NB_EPOLL_TAG_UDP 1ULL
#define NB_EPOLL_TAG_CLIENT_UDP 5ULL
#define NB_EPOLL_TAG_DNS 2ULL
#define NB_EPOLL_TAG_LISTEN 3ULL
#define NB_EPOLL_TAG_CONTROL 4ULL
#define NB_EPOLL_TAG_AUTH 6ULL
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

#define DNS_WORKERS 4
#define AUTH_WORKERS 4
typedef nb_dns_result_t dns_res_t;

static int queue_down(proxy_stream_t* p, const uint8_t* d, size_t n, int fin);
static void middle_upstream_fc_set(proxy_stream_t* p, int enabled);
static void middle_upstream_fc_update(proxy_stream_t* p, uint64_t now_us);
static int queue_up(proxy_stream_t* p, const uint8_t* d, size_t n, int fin);
static void fwd_down(proxy_stream_t* p, uint8_t* d, size_t n, int fin);
static void fwd_up(proxy_stream_t* p, uint8_t* d, size_t n, int fin);
static int pool_init(cnx_pool_t* pool, struct sockaddr_storage* addr);
static int pool_get_or_create(struct sockaddr_storage* addr);
static int downstream_open_stream(proxy_stream_t* p, const char* route, const nb_flow_policy_t* pol);
#define parse_port_strict nb_bridge_parse_port
#define parse_target_from_route nb_bridge_parse_target
#define fec_parse_target_route nb_bridge_parse_target_exact
#define udp_route_consume_h nb_bridge_consume_hop
#define dns_submit(id,host,port) nb_dns_context_submit(nb_current_instance->dns,(id),(host),(port))
#define dns_pop_result(result) nb_dns_context_pop(nb_current_instance->dns,(result))
static int epoll_tcp_update(proxy_stream_t* p, int add);

/* ---- 工具 ---- */
static int set_nonblock(int fd){ int fl=fcntl(fd,F_GETFL,0); return fl<0?-1:fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
static const char* role_name(nb_role_t r){ return nb_bridge_role_name((int)r); }

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

static const char* control_env_id(const char* name,const char* fallback){
    const char* value=nb_instance_env(nb_current_instance,name);if(value==NULL||value[0]==0)return fallback;
    for(const unsigned char* p=(const unsigned char*)value;*p;p++){
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='.'||*p=='-'))return fallback;
    }
    return value;
}

static uint64_t env_ms_to_us(const char* name,uint64_t fallback_ms,uint64_t minimum_ms,uint64_t maximum_ms){
    const char* value=nb_instance_env(nb_current_instance,name);if(value==NULL||value[0]==0)return fallback_ms*1000ULL;
    char* end=NULL;errno=0;unsigned long long parsed=strtoull(value,&end,10);
    if(errno||end==value||*end||parsed<minimum_ms||parsed>maximum_ms){
        log4c_warn("ignore invalid %s=%s range=%llu..%llums",name,value,
            (unsigned long long)minimum_ms,(unsigned long long)maximum_ms);
        return fallback_ms*1000ULL;
    }
    return (uint64_t)parsed*1000ULL;
}

static int configure_udp_socket_buffers(int fd,const char* label){
    int requested=16*1024*1024,receive=0,send=0;
    if(nb_udp_configure_buffers(fd,requested,&receive,&send)!=0){
        log4c_error("%s udp buffer configuration failed: %s",
            label?label:"quic",strerror(errno));return -1;
    }
    log4c_info("%s udp buffers rcvbuf=%d sndbuf=%d requested=%d",
        label?label:"quic",receive,send,requested);
    return 0;
}

static int instance_transport_ready(void){
    if(G.role!=ROLE_ENTRY)return 1;
    if(G.pool_count==0)return 0;
    for(int pool_id=0;pool_id<G.pool_count;pool_id++){
        cnx_pool_t* pool=&G.pools[pool_id];int ready=0;
        if(!pool->configured)continue;
        for(int slot=0;slot<POOL_SIZE;slot++)if(pool->cnx[slot]){
            picoquic_state_enum state=picoquic_get_cnx_state(pool->cnx[slot]);
            if(state==picoquic_state_client_ready_start||state==picoquic_state_ready){ready=1;break;}
        }
        if(!ready)return 0;
    }
    return 1;
}

static int control_render(const char* command,char* out,size_t cap,void* ctx){
    (void)ctx;
    const char* worker=nb_instance_env(nb_current_instance,"NB_WORKER_ID");if(!worker)worker="0";
    const char* release=control_env_id("NB_RELEASE_ID","unversioned");
    const char* binary_release=control_env_id("NB_BINARY_RELEASE_ID",release);
    const char* profile=control_env_id("NB_LINE_PROFILE_ID","unversioned");
    const char* schema_text=nb_instance_env(nb_current_instance,"NB_LINE_PROFILE_SCHEMA");char* schema_end=NULL;
    long schema_long=schema_text?strtol(schema_text,&schema_end,10):0;
    int profile_schema=(schema_text&&schema_end&&*schema_end==0&&schema_long>=0&&schema_long<=999999)?(int)schema_long:0;
    if(!strcmp(command,"health")||!strcmp(command,"GET /health")){
        uint64_t logical[NB_LSTREAM_FIN_ACK+1]={0};
        for(int i=0;i<MAX_CONN;i++)if(G.streams[i].in_use)for(int type=NB_LSTREAM_OPEN;type<=NB_LSTREAM_FIN_ACK;type++)
            logical[type]+=G.streams[i].logical_rx_frames[type];
        const char* status=instance_transport_ready()?"ok":"starting";
        return snprintf(out,cap,"{\"status\":\"%s\",\"role\":\"%s\",\"worker\":\"%s\",\"boot_id\":\"%016llx\",\"release_id\":\"%s\",\"binary_release_id\":\"%s\",\"line_profile\":\"%s\",\"line_profile_schema\":%d,\"resources\":{\"sessions\":%d,\"sessions_limit\":%u,\"queue_bytes\":%llu,\"queue_limit_bytes\":%llu},\"logical_rx\":{\"open\":%llu,\"data\":%llu,\"ack\":%llu,\"resume\":%llu,\"resume_ack\":%llu,\"fin\":%llu,\"rst\":%llu,\"fin_ack\":%llu}}\n",
            status,role_name(G.role),worker,(unsigned long long)g_run_id,release,binary_release,profile,profile_schema,g_ps_inuse,
            nb_current_instance->session_limit,(unsigned long long)nb_current_instance->queue_bytes,
            (unsigned long long)nb_current_instance->queue_limit_bytes,
            (unsigned long long)logical[NB_LSTREAM_OPEN],(unsigned long long)logical[NB_LSTREAM_DATA],
            (unsigned long long)logical[NB_LSTREAM_ACK],(unsigned long long)logical[NB_LSTREAM_RESUME],
            (unsigned long long)logical[NB_LSTREAM_RESUME_ACK],(unsigned long long)logical[NB_LSTREAM_FIN],
            (unsigned long long)logical[NB_LSTREAM_RST],(unsigned long long)logical[NB_LSTREAM_FIN_ACK]);
    }
    if(!strcmp(command,"metrics")||!strcmp(command,"GET /metrics")){
        uint64_t now=picoquic_current_time();nb_metrics_snapshot_t snapshot;
        nb_metrics_snapshot_init(&snapshot,&g_metrics_state,(uint64_t)g_ps_inuse,(uint64_t)g_ps_peak,
            (uint64_t)G.pool_count,(uint64_t)G.exit_routes.count);
        snapshot.sessions_limit=nb_current_instance->session_limit;
        snapshot.queue_bytes_used=nb_current_instance->queue_bytes;
        snapshot.queue_bytes_limit=nb_current_instance->queue_limit_bytes;
        for(int pool=0;pool<G.pool_count;pool++)for(int slot=0;slot<POOL_SIZE;slot++){
            cnx_pool_t* cp=&G.pools[pool];
            if(cp->cnx[slot]&&cp->recent_sent[slot]>0)nb_metrics_snapshot_add_link(&snapshot,
                cp->recent_loss[slot],cp->recent_rtt[slot],cp->recent_rtt_var[slot],cp->recent_sent[slot]);
            if(cp->cnx[slot]){
                picoquic_cnx_t* cnx=cp->cnx[slot];picoquic_path_quality_t quality;memset(&quality,0,sizeof(quality));
                picoquic_get_default_path_quality(cnx,&quality);uint64_t cc_state=0,cc_parameter=0;
                picoquic_path_t* path=(cnx->nb_paths>0)?cnx->path[0]:NULL;
                if(path&&path->congestion_alg_state&&cnx->congestion_alg&&cnx->congestion_alg->alg_observe)
                    cnx->congestion_alg->alg_observe(path,&cc_state,&cc_parameter);
                const char* cc=(cnx->congestion_alg&&cnx->congestion_alg->congestion_algorithm_id)?cnx->congestion_alg->congestion_algorithm_id:"none";
                nb_metrics_snapshot_add_transport(&snapshot,cc,cc_state,quality.lost,quality.spurious_losses,
                    quality.timer_losses,quality.max_reorder_gap,quality.max_reorder_delay,quality.cwin,
                    quality.bytes_in_transit,quality.pacing_rate,cnx->cwin_blocked||cnx->flow_blocked||cnx->stream_blocked);
            }
        }
        nb_fec_metrics_t fm=g_fec_metrics_done;
        for(int i=0;i<MAX_CONN;i++){
            proxy_stream_t* p=&G.streams[i];if(!p->in_use)continue;
            size_t down=p->udp_mode?p->udp_down_tx_len:p->down_tx.len;
            size_t up=p->udp_mode?p->udp_up_tx_len:p->up_tx.len;
            size_t q2t=p->udp_mode?p->udp_pending_tx_len:p->q2t.len;
            uint64_t down_age=p->udp_mode?0:nb_live_queue_age_us(&p->queue_clocks->down_tx,p->down_tx.len,now);
            uint64_t up_age=p->udp_mode?0:nb_live_queue_age_us(&p->queue_clocks->up_tx,p->up_tx.len,now);
            uint64_t q2t_age=p->udp_mode?0:nb_live_queue_age_us(&p->queue_clocks->q2t,p->q2t.len,now);
            uint64_t c2s_wait=(p->first_c2s_at?p->first_c2s_at:now)-p->created_at;
            uint64_t s2c_wait=(p->first_s2c_at?p->first_s2c_at:now)-p->created_at;
            nb_metrics_snapshot_add_stream(&snapshot,(int)p->flow_class,p->udp_mode,p->tcp_connecting,
                p->tcp_read_paused,p->upstream_fc_blocked,p->target_connect_state==3,p->bytes_c2s,p->bytes_s2c,
                down,up,q2t,down_age,up_age,q2t_age,c2s_wait,s2c_wait);
            if(p->fec_engine!=NULL)fec_metrics_add(&fm,nb_fec_metrics(p->fec_engine));
        }
        nb_metrics_fec_t fec={g_fec_v15_observe,g_fec_v15_active,fm.blocks_encoded,fm.blocks_delivered,
            fm.blocks_recovered,fm.nack_sent,fm.retx_sent};
        return nb_metrics_render_json(out,cap,role_name(G.role),worker,release,profile,profile_schema,&snapshot,&fec);
    }
    if(!strcmp(command,"tenants")||!strcmp(command,"GET /tenants"))return nb_tenants_render_json(&g_tenants,out,cap);
    if(!strcmp(command,"routes")||!strcmp(command,"GET /routes"))return nb_routes_render_json(&G.exit_routes,out,cap,picoquic_current_time());
    int route_command=nb_routes_control_command(&G.exit_routes,command,out,cap,picoquic_current_time(),30000000ULL);
    if(route_command>=0)return route_command;
    return -1;
}


/* ===== 白名单(访问控制) =====
 * entry 收到 SOCKS CONNECT(host:port): 命中(域名后缀 / IP CIDR / 端口)才允许走隧道, 未命中直接拒绝。
 * 目的: 防止无关流量走三跳线路占带宽 + 降低安全风险。启动加载 + 文件 mtime 热重载。entry 单线程, 无锁。
 * 规则: host命中(域名或IP) AND port命中; 某类白名单为空 = 该维度不限制。 */
static int whitelist_allowed(const char* host, int port){
    if(port==NB_PROBE_PORT && host!=NULL &&
        (!strcasecmp(host,NB_PROBE_SINK_HOST)||!strcasecmp(host,NB_PROBE_ECHO_HOST)))return 1;
    return nb_whitelist_context_allowed(nb_current_instance->whitelist,host,port);
}

static void flow_policy_for_host(const char* host, int port, nb_flow_policy_t* out){
    nb_flow_policy_default(out);
    if(port==NB_PROBE_PORT && host!=NULL &&
        (!strcasecmp(host,NB_PROBE_SINK_HOST)||!strcasecmp(host,NB_PROBE_ECHO_HOST))){
        out->matched=1;out->flow_class=NB_FLOW_CLASS_MEDIA;out->lane_hint=NB_FLOW_LANE_LATENCY;
        out->fec_hint=NB_FLOW_FEC_OFF;out->prio=NB_PRIO_MEDIA;
        snprintf(out->rule_name,sizeof(out->rule_name),"internal-probe");
        return;
    }
    (void)nb_policy_context_classify(nb_current_instance->policy,host,port,out);
}

static void apply_flow_policy(proxy_stream_t* p, const nb_flow_policy_t* pol){
    p->flow_class = pol->flow_class;
    p->lane_hint = pol->lane_hint;
    p->fec_hint = pol->fec_hint;
    p->prio = pol->prio;
    snprintf(p->flow_rule, sizeof(p->flow_rule), "%s", pol->rule_name);
    if(pol->flow_class==NB_FLOW_CLASS_MEDIA){
        if(p->up_cnx)picoquic_set_media_mode(p->up_cnx,1);
        if(p->down_cnx)picoquic_set_media_mode(p->down_cnx,1);
    }
    middle_upstream_fc_set(p,pol->flow_class==NB_FLOW_CLASS_MEDIA);
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

static void ps_promote_media(proxy_stream_t* p,const char* rule,double rate_kbps){
    if(p==NULL||p->flow_class==NB_FLOW_CLASS_MEDIA)return;
    p->flow_class=NB_FLOW_CLASS_MEDIA;p->lane_hint=NB_FLOW_LANE_LATENCY;
    p->fec_hint=NB_FLOW_FEC_AUTO;p->prio=NB_PRIO_MEDIA;
    snprintf(p->flow_rule,sizeof(p->flow_rule),"%s",rule);
    if(p->up_cnx&&!p->udp_mode)picoquic_set_stream_priority(p->up_cnx,p->up_stream_id,NB_PRIO_MEDIA);
    if(p->up_cnx)picoquic_set_media_mode(p->up_cnx,1);
    if(p->down_cnx&&!p->udp_mode&&p->down_opened)picoquic_set_stream_priority(p->down_cnx,p->down_stream_id,NB_PRIO_MEDIA);
    if(p->down_cnx)picoquic_set_media_mode(p->down_cnx,1);
    middle_upstream_fc_set(p,1);
    log4c_info("%s flow auto-promote id=%u rate_c2s=%.1fKbps rule=%s route=%s",
        role_name(G.role),p->id,rate_kbps,rule,p->route);
}

static void ps_demote_bulk(proxy_stream_t* p,const char* rule,double rate_kbps){
    if(p==NULL||p->flow_class!=NB_FLOW_CLASS_CTRL)return;
    p->flow_class=NB_FLOW_CLASS_BULK;p->lane_hint=NB_FLOW_LANE_BULK;
    p->fec_hint=NB_FLOW_FEC_OFF;p->prio=NB_PRIO_BULK;
    snprintf(p->flow_rule,sizeof(p->flow_rule),"%s",rule);
    if(p->up_cnx&&!p->udp_mode)picoquic_set_stream_priority(p->up_cnx,p->up_stream_id,NB_PRIO_BULK);
    if(p->down_cnx&&!p->udp_mode&&p->down_opened)picoquic_set_stream_priority(p->down_cnx,p->down_stream_id,NB_PRIO_BULK);
    middle_upstream_fc_set(p,0);
    log4c_info("%s flow auto-demote id=%u rate=%.1fKbps rule=%s route=%s",
        role_name(G.role),p->id,rate_kbps,rule,p->route);
}

static void ps_update_runtime_policy(proxy_stream_t* p,uint64_t now){
    if(g_tenants_enabled&&p->tenant_index>=0){nb_tenant_account(&g_tenants,p->tenant_index,p->bytes_c2s-p->tenant_accounted_up,p->bytes_s2c-p->tenant_accounted_down);p->tenant_accounted_up=p->bytes_c2s;p->tenant_accounted_down=p->bytes_s2c;}
    double c2s=0,s2c=0;
    nb_live_flow_action_t action=nb_live_flow_observe(&p->live_runtime,p->flow_class,
        p->bytes_c2s,p->bytes_s2c,now,&c2s,&s2c);
    if(action==NB_LIVE_FLOW_PROMOTE_MEDIA)ps_promote_media(p,"auto-high-uplink",c2s);
    else if(action==NB_LIVE_FLOW_DEMOTE_BULK)ps_demote_bulk(p,"auto-high-throughput-ctrl",c2s>s2c?c2s:s2c);
}

static void ps_note_payload(proxy_stream_t* p, int c2s, size_t len){
    if(p == NULL || len == 0) return;
    uint64_t now = picoquic_current_time();
    p->last_active = now;
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
    if(c2s&&!p->udp_mode&&p->flow_class==NB_FLOW_CLASS_BULK&&
        strcmp(p->flow_rule,"raw-ip-443")==0){
        uint64_t age=now-p->created_at;
        if(age>0&&age<=2000000ULL&&p->bytes_c2s>=64*1024){
            double rate_kbps=(double)p->bytes_c2s*8000.0/(double)age;
            if(rate_kbps>=256.0)ps_promote_media(p,"fast-high-uplink",rate_kbps);
        }
    }
}

#define NB_MEDIA_METRICS_INTERVAL_US 5000000ULL
static void ps_log_media_metrics(proxy_stream_t* p,uint64_t now){
    if(p==NULL||!p->in_use)return;
    size_t down_q=p->udp_mode?p->udp_down_tx_len:p->down_tx.len;
    size_t up_q=p->udp_mode?p->udp_up_tx_len:p->up_tx.len;
    size_t q2t_q=p->udp_mode?p->udp_pending_tx_len:p->q2t.len;
    if(p->media_metrics_at==0){
        p->media_metrics_at=now;p->media_metrics_c2s=p->bytes_c2s;p->media_metrics_s2c=p->bytes_s2c;
        p->media_down_q_peak=down_q;p->media_up_q_peak=up_q;p->media_q2t_peak=q2t_q;
        return;
    }
    uint64_t elapsed=now-p->media_metrics_at;
    if(elapsed<NB_MEDIA_METRICS_INTERVAL_US)return;
    uint64_t c2s_delta=p->bytes_c2s-p->media_metrics_c2s;
    uint64_t s2c_delta=p->bytes_s2c-p->media_metrics_s2c;
    double c2s_kbps=(double)c2s_delta*8000.0/(double)elapsed;
    double s2c_kbps=(double)s2c_delta*8000.0/(double)elapsed;
    int known_media=ps_is_rtc_webcast(p);
    int high_throughput=c2s_kbps>=256.0||s2c_kbps>=256.0;
    size_t prefix_q=p->down_prefix_len>p->down_prefix_off?p->down_prefix_len-p->down_prefix_off:0;
    double down_age=nb_live_queue_age_us(&p->queue_clocks->down_tx,p->down_tx.len,now)/1000.0;
    double up_age=nb_live_queue_age_us(&p->queue_clocks->up_tx,p->up_tx.len,now)/1000.0;
    double q2t_age=nb_live_queue_age_us(&p->queue_clocks->q2t,p->q2t.len,now)/1000.0;
    if(known_media||high_throughput){
        log4c_info("%s %s flow stat id=%u class=%s rule=%s age=%.1fs window=%.1fs rate_kbps=c2s:%.1f,s2c:%.1f total_kb=c2s:%llu,s2c:%llu queue_now=prefix:%zu,down:%zu,up:%zu,q2t:%zu queue_age_ms=down:%.1f,up:%.1f,q2t:%.1f queue_peak=down:%zu,up:%zu,q2t:%zu tcp_paused=%d route=%s",
            role_name(G.role),known_media?"media":"high-throughput",p->id,nb_flow_class_name(p->flow_class),p->flow_rule,
            (now-p->created_at)/1000000.0,elapsed/1000000.0,c2s_kbps,s2c_kbps,
            (unsigned long long)(p->bytes_c2s/1024),(unsigned long long)(p->bytes_s2c/1024),
            prefix_q,down_q,up_q,q2t_q,down_age,up_age,q2t_age,p->media_down_q_peak,p->media_up_q_peak,
            p->media_q2t_peak,p->tcp_read_paused,p->route);
    }
    p->media_metrics_at=now;p->media_metrics_c2s=p->bytes_c2s;p->media_metrics_s2c=p->bytes_s2c;
    p->media_down_q_peak=down_q;p->media_up_q_peak=up_q;p->media_q2t_peak=q2t_q;
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
