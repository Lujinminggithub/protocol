/* 运行时主循环：把 TUN、UDP/AF_XDP 与数据面逻辑串接起来。 */

#define _POSIX_C_SOURCE 200809L

#include "xgw_runtime.h"

#include "xgw_afxdp.h"
#include "xgw_bridge.h"
#include "xgw_local_adapter.h"
#include "xgw_control.h"

#include "log4c.h"
#include "xgw_dataplane.h"
#include "xgw_obfs.h"
#include "xgw_outbound.h"
#include "xgw_route.h"
#include "xgw_transport.h"
#include "xgw_tun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void xgw_sleep_ms(unsigned int ms) { Sleep(ms); }
static void xgw_sleep_us(uint64_t us) {
    unsigned int ms = (unsigned int) ((us + 999ULL) / 1000ULL);
    if (ms == 0U) {
        ms = 1U;
    }
    Sleep(ms);
}
#else
#include <time.h>
static void xgw_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (long) (ms % 1000U) * 1000000L;
    nanosleep(&ts, NULL);
}
static void xgw_sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (time_t) (us / 1000000ULL);
    ts.tv_nsec = (long) ((us % 1000000ULL) * 1000ULL);
    nanosleep(&ts, NULL);
}
#endif

#define XGW_BRIDGE_TARGET_MAP_SIZE 1024U
#define XGW_FLOW_PRIORITY_HIGH 2U
#define XGW_FLOW_PRIORITY_NORMAL 1U
#define XGW_FLOW_PRIORITY_LOW 0U

/* P0 延迟测量：进程级累计统计，周期 dump 定位 gap 来源（cc_stall=发送门控阻塞 vs queue_wait=调度排队）。
 * 全局 static 累加，避免改 cc 结构；按 slot 切分以区分 control/bootstrap 流。 */
typedef struct xgw_p0_lat {
    uint64_t cc_stall_us;        /* maybe_pace 因 !can_send 自旋/sleep 的累计微秒 */
    uint64_t cc_stall_count;     /* 触发自旋的次数 */
    uint64_t cc_stall_max_us;    /* 单次最长阻塞 */
    uint64_t pace_wait_us;       /* pacing next_send_time 等待累计 */
    uint64_t queue_wait_us_sum;  /* stream_sched 入队到出队累计 */
    uint64_t queue_wait_count;
    uint64_t queue_wait_max_us;  /* 单流最长排队 */
} xgw_p0_lat_t;
static xgw_p0_lat_t g_p0_prev;     /* PREVIOUS(回程)方向 */
static xgw_p0_lat_t g_p0_next;     /* NEXT(去程)方向 */

static void p0_note_cc_stall(uint64_t stall_us, int spun) {
    if (spun) {
        g_p0_prev.cc_stall_count++;
        if (stall_us > g_p0_prev.cc_stall_max_us) g_p0_prev.cc_stall_max_us = stall_us;
    }
    g_p0_prev.cc_stall_us += stall_us;
}
static void p0_note_queue_wait(uint64_t wait_us, int is_return) {
    xgw_p0_lat_t *p = is_return ? &g_p0_prev : &g_p0_next;
    p->queue_wait_us_sum += wait_us;
    p->queue_wait_count++;
    if (wait_us > p->queue_wait_max_us) p->queue_wait_max_us = wait_us;
}
static void p0_dump(void) {
    printf("runtime.p0.latency dir=prev cc_stall_us=%llu cc_stall_n=%llu cc_stall_max_us=%llu queue_wait_avg_us=%llu queue_wait_max_us=%llu queue_n=%llu\n",
           (unsigned long long) g_p0_prev.cc_stall_us,
           (unsigned long long) g_p0_prev.cc_stall_count,
           (unsigned long long) g_p0_prev.cc_stall_max_us,
           (unsigned long long) (g_p0_prev.queue_wait_count ? g_p0_prev.queue_wait_us_sum / g_p0_prev.queue_wait_count : 0ULL),
           (unsigned long long) g_p0_prev.queue_wait_max_us,
           (unsigned long long) g_p0_prev.queue_wait_count);
    printf("runtime.p0.latency dir=next queue_wait_avg_us=%llu queue_wait_max_us=%llu queue_n=%llu\n",
           (unsigned long long) (g_p0_next.queue_wait_count ? g_p0_next.queue_wait_us_sum / g_p0_next.queue_wait_count : 0ULL),
           (unsigned long long) g_p0_next.queue_wait_max_us,
           (unsigned long long) g_p0_next.queue_wait_count);
    fflush(stdout);
}
#define XGW_STREAM_SCHED_CAP 512U
#define XGW_STREAM_SCHED_PAYLOAD_MAX 65535U

typedef struct xgw_traffic_limiter {
    int enabled;
    uint64_t baseline_bps;
    uint64_t burst_bps;
    uint64_t capacity_bytes;
    double tokens;
    uint64_t last_refill_us;
    uint64_t next_peak_send_us;
} xgw_traffic_limiter_t;

typedef struct xgw_bridge_target_entry {
    int active;
    uint32_t stream_id;
    char target_host[256];
    uint16_t target_port;
    char frontend[16];
    char front_session_id[40];
    char route_name[24];
    char line_id[16];
    uint8_t flow_class;
    uint8_t priority;
    uint8_t budget_flags;
    uint8_t preferred_copies;
    uint32_t read_timeout_ms;
    uint32_t idle_after_first_byte_ms;
    uint64_t opened_us;
    uint64_t relay_first_packet_us;
    uint64_t relay_first_byte_us;
    char close_reason[64];
} xgw_bridge_target_entry_t;

typedef struct xgw_stream_sched_entry {
    int active;
    size_t slot_pos; /* index into sched->active_idx[] for O(1) swap-remove */
    uint32_t stream_id;
    char target_host[256];
    uint16_t target_port;
    char frontend[16];
    char front_session_id[40];
    char route_name[24];
    char line_id[XGW_MAX_NAME_LEN];
    xgw_channel_direction_t send_direction;
    xgw_session_slot_t session_slot;
    xgw_flow_hint_t hint;
    uint8_t payload[XGW_STREAM_SCHED_PAYLOAD_MAX];
    size_t payload_len;
    uint8_t pending;
    uint64_t enqueued_us;
    uint64_t last_sent_us;
    uint64_t last_credit_refill_us;
    uint64_t first_return_us;
    uint64_t last_return_us;
    uint64_t send_credit_bytes;
    uint64_t inflight_bytes;
    uint64_t delivered_return_bytes;
    uint64_t sent_bytes_total;
    uint32_t ack_credit_frames;
    uint64_t deficit_bytes;
    uint32_t starvation_rounds;
    uint32_t rolling_loss_ppm;
    uint32_t rolling_recovered_ppm;
    uint8_t parity_budget;
    uint16_t feedback_cadence_ms;
} xgw_stream_sched_entry_t;

/* ---- O(N) 调度聚合：每次 pick 预聚合分组量，消除 stream_sched_pick 的 O(N^2~N^3) strcmp。
 * 用容斥统计「同 session(fsid) ∪ 同 route ∪ 同 line(R/L 均含 dir+slot)」分组的 pending 数与
 * inflight 字节；另用 (line,dir) 桶统计 total_weight。哈希用 FNV64，512 流下碰撞可忽略，
 * selftest 对拍 _ref 旧实现保证逐值等价。*/
#define SCHED_AGG_SLOTS 1024U /* 必须为 2 的幂；active 上限 512，负载 ≤0.5 */
typedef struct sched_agg_slot {
    uint64_t key;
    uint64_t cnt;   /* 该桶内 pending!=0 的流数 */
    uint64_t sum;   /* 该桶内 active 流的 inflight_bytes 累加 */
    uint32_t wsum;  /* weight 累加（仅 W 桶用） */
    uint32_t gen;   /* 代际：!= agg.gen 视为空槽，免每次 pick memset */
} sched_agg_slot_t;
typedef struct sched_agg_map {
    sched_agg_slot_t slots[SCHED_AGG_SLOTS];
} sched_agg_map_t;
typedef struct stream_sched_agg {
    int valid;
    uint32_t gen;
    sched_agg_map_t s, r, l, sr, sl, rl, srl, w;
} stream_sched_agg_t;
typedef struct sched_group_acc {
    uint64_t cnt;
    uint64_t sum;
} sched_group_acc_t;

typedef struct xgw_stream_scheduler {
    xgw_stream_sched_entry_t entries[XGW_STREAM_SCHED_CAP];
    size_t active_idx[XGW_STREAM_SCHED_CAP];
    size_t active_n;
    size_t rr_cursor;
    stream_sched_agg_t agg;
} xgw_stream_scheduler_t;

typedef struct xgw_runtime_line_channels {
    int valid;
    char line_id[XGW_MAX_NAME_LEN];
    xgw_channel_set_t channels;
} xgw_runtime_line_channels_t;

typedef struct xgw_runtime_channels {
    xgw_runtime_line_channels_t active;
    xgw_runtime_line_channels_t candidate;
    xgw_runtime_line_channels_t draining;
} xgw_runtime_channels_t;

static uint64_t stream_sched_budget_bytes(const xgw_stream_sched_entry_t *entry);
static uint64_t stream_sched_session_budget_bytes(const xgw_stream_scheduler_t *sched,
                                                  const xgw_stream_sched_entry_t *entry,
                                                  const xgw_session_t *session);
static uint64_t stream_sched_session_refill_rate_bytes_per_sec(const xgw_stream_scheduler_t *sched,
                                                               const xgw_stream_sched_entry_t *entry,
                                                               const xgw_session_t *session);
static void stream_sched_agg_rebuild(xgw_stream_scheduler_t *sched);
static void stream_sched_refill_credit(const xgw_stream_scheduler_t *sched,
                                       xgw_stream_sched_entry_t *entry,
                                       const xgw_session_t *session,
                                       uint64_t now_us);
static const char *runtime_session_slot_name(xgw_session_slot_t slot);
static xgw_session_t *get_send_session(xgw_dataplane_t *dp,
                                       const xgw_runtime_config_t *config,
                                       const char *line_id,
                                       xgw_channel_direction_t direction,
                                       xgw_session_slot_t slot,
                                       const char *host,
                                       uint16_t port);
static void runtime_session_segment_key(const xgw_runtime_config_t *config,
                                        xgw_channel_direction_t direction,
                                        char *out,
                                        size_t out_len);
static xgw_transport_segment_t runtime_transport_segment(const xgw_runtime_config_t *config,
                                                         xgw_channel_direction_t direction);
static void runtime_channel_refresh(xgw_runtime_channels_t *channels,
                                    const xgw_route_manager_t *routes);
static xgw_session_t *runtime_channel_session(xgw_runtime_channels_t *channels,
                                              const char *line_id,
                                              xgw_channel_direction_t direction,
                                              xgw_session_slot_t slot,
                                              int require_ready);
static xgw_session_t *runtime_channel_session_for_sched(xgw_runtime_channels_t *channels,
                                                        const char *line_id,
                                                        xgw_channel_direction_t direction,
                                                        xgw_session_slot_t slot,
                                                        int require_ready);
static int maybe_send_keepalive(xgw_udp_socket_t *udp,
                                xgw_session_t *session,
                                const xgw_runtime_config_t *config,
                                const xgw_obfs_t *obfs,
                                int hop_obfs,
                                const char *host,
                                uint16_t port);
static void maybe_refresh_control_session(xgw_session_t *session,
                                          const xgw_runtime_config_t *config,
                                          const char *host,
                                          uint16_t port);
static int maybe_send_handshake(xgw_udp_socket_t *udp,
                                xgw_session_t *session,
                                const xgw_runtime_config_t *config,
                                const xgw_negotiation_info_t *negotiation,
                                const xgw_obfs_t *obfs,
                                int hop_obfs,
                                const char *host,
                                uint16_t port);

static uint64_t max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

/* PMTUD：UDP 发送遇到 EMSGSIZE 时置位，主循环每轮把它喂给 dataplane 做回退。
 * 用模块级标志而非给 send_frame_series_udp 加参数，改动面更小。 */
static int g_pmtu_too_big_pending = 0;

/* 全局日志级别（从 config.tuning.log_level 初始化）：
 * 0=安静(仅错误/启动)，1=正常(默认，关键事件)，2+=verbose(逐包调试)。
 * 逐包热路径日志用 XGW_RT_VERBOSE() 门控，避免写满磁盘（曾把 relay/egress 撑到 100%）。 */
static uint32_t g_xgw_log_level = 1U;
#define XGW_RT_VERBOSE() (g_xgw_log_level >= 2U)
#define XGW_RT_NORMAL()  (g_xgw_log_level >= 1U)

static uint64_t monotonic_us(void);

static uint32_t classify_target_priority_runtime(const char *host, uint16_t port);

static void set_error(char *error, size_t error_len, const char *text) {
    if (error_len > 0U) {
        snprintf(error, error_len, "%s", text == NULL ? "" : text);
    }
}

static uint64_t now_us(void) {
    return monotonic_us();
}

static uint64_t monotonic_us(void) {
#ifdef _WIN32
    return (uint64_t) GetTickCount64() * 1000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000000ULL) + ((uint64_t) ts.tv_nsec / 1000ULL);
#endif
}

static uint64_t monotonic_ns(void) {
    return monotonic_us() * 1000ULL;
}

static uint32_t stable_hash32_runtime(const char *text) {
    uint32_t h = 2166136261U;
    const unsigned char *p = (const unsigned char *) (text == NULL ? "" : text);
    while (*p != '\0') {
        h ^= (uint32_t) (*p++);
        h *= 16777619U;
    }
    return h;
}

static void traffic_limiter_init(xgw_traffic_limiter_t *limiter, const xgw_runtime_config_t *config) {
    uint64_t baseline_bps;
    uint64_t burst_bps;
    uint32_t burst_seconds;
    uint64_t capacity_source_bps;
    if (limiter == NULL) {
        return;
    }
    memset(limiter, 0, sizeof(*limiter));
    if (config == NULL) {
        return;
    }
    baseline_bps = config->tuning.traffic_baseline_bps;
    burst_bps = config->tuning.traffic_burst_bps;
    burst_seconds = config->tuning.traffic_burst_seconds;
    if (baseline_bps == 0U && burst_bps == 0U) {
        return;
    }
    if (baseline_bps == 0U) {
        baseline_bps = burst_bps;
    }
    if (burst_bps == 0U || burst_bps < baseline_bps) {
        burst_bps = baseline_bps;
    }
    if (burst_seconds == 0U) {
        burst_seconds = 600U;
    }
    capacity_source_bps = burst_bps > baseline_bps ? burst_bps - baseline_bps : burst_bps;
    limiter->enabled = 1;
    limiter->baseline_bps = baseline_bps;
    limiter->burst_bps = burst_bps;
    limiter->capacity_bytes = (capacity_source_bps / 8ULL) * (uint64_t) burst_seconds;
    if (limiter->capacity_bytes == 0U) {
        limiter->capacity_bytes = burst_bps / 8ULL;
    }
    limiter->tokens = (double) limiter->capacity_bytes;
    limiter->last_refill_us = monotonic_us();
}

static void traffic_limiter_refill(xgw_traffic_limiter_t *limiter, uint64_t now) {
    double refill;
    uint64_t elapsed;
    if (limiter == NULL || !limiter->enabled || limiter->last_refill_us == 0U || now <= limiter->last_refill_us) {
        return;
    }
    elapsed = now - limiter->last_refill_us;
    refill = ((double) limiter->baseline_bps / 8.0) * ((double) elapsed / 1000000.0);
    limiter->tokens += refill;
    if (limiter->tokens > (double) limiter->capacity_bytes) {
        limiter->tokens = (double) limiter->capacity_bytes;
    }
    limiter->last_refill_us = now;
}

static void traffic_limiter_consume(xgw_traffic_limiter_t *limiter, size_t bytes) {
    uint64_t now;
    uint64_t peak_gap_us = 0U;
    uint64_t sleep_us = 0U;
    if (limiter == NULL || !limiter->enabled || bytes == 0U) {
        return;
    }
    now = monotonic_us();
    traffic_limiter_refill(limiter, now);
    if (limiter->burst_bps > 0U) {
        peak_gap_us = ((uint64_t) bytes * 8ULL * 1000000ULL) / limiter->burst_bps;
        if (limiter->next_peak_send_us > now) {
            sleep_us = limiter->next_peak_send_us - now;
        }
    }
    if (limiter->baseline_bps > 0U && limiter->tokens < (double) bytes) {
        double deficit = (double) bytes - limiter->tokens;
        uint64_t baseline_sleep = (uint64_t) ((deficit * 8.0 * 1000000.0) / (double) limiter->baseline_bps);
        if (baseline_sleep > sleep_us) {
            sleep_us = baseline_sleep;
        }
    }
    if (sleep_us > 0U) {
        unsigned int sleep_ms = (unsigned int) ((sleep_us + 999ULL) / 1000ULL);
        if (sleep_ms == 0U) {
            sleep_ms = 1U;
        }
        xgw_sleep_ms(sleep_ms);
        now = monotonic_us();
        traffic_limiter_refill(limiter, now);
    }
    if (limiter->tokens >= (double) bytes) {
        limiter->tokens -= (double) bytes;
    } else {
        limiter->tokens = 0.0;
    }
    if (peak_gap_us > 0U) {
        limiter->next_peak_send_us = now + peak_gap_us;
    }
}

static uint32_t read_be32_runtime(const uint8_t *p) {
    return ((uint32_t) p[0] << 24U) |
           ((uint32_t) p[1] << 16U) |
           ((uint32_t) p[2] << 8U) |
           (uint32_t) p[3];
}

static uint16_t read_be16_runtime(const uint8_t *p) {
    return (uint16_t) (((uint16_t) p[0] << 8U) | (uint16_t) p[1]);
}

static uint32_t read_bridge_timeout_runtime(const uint8_t *meta, size_t meta_len, size_t offset_v1, size_t offset_v2) {
    if (meta == NULL || meta_len < 8U) {
        return 0U;
    }
    if (meta_len >= 12U) {
        return read_be32_runtime(meta + offset_v2);
    }
    return (uint32_t) read_be16_runtime(meta + offset_v1);
}

static unsigned long long elapsed_ms_runtime(uint64_t start_us) {
    uint64_t now = monotonic_us();
    if (start_us == 0U || now <= start_us) {
        return 0ULL;
    }
    return (unsigned long long) ((now - start_us) / 1000ULL);
}

static int contains_ignore_case_runtime(const char *text, const char *needle) {
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

static int parse_bridge_payload_meta(const uint8_t *payload,
                                     size_t payload_len,
                                     uint32_t *stream_id,
                                     uint8_t *kind,
                                     uint16_t *data_len) {
    if (payload == NULL || payload_len < 9U) {
        return 0;
    }
    if (read_be32_runtime(payload) != 0x58474231U) {
        return 0;
    }
    if (stream_id != NULL) {
        *stream_id = read_be32_runtime(payload + 4U);
    }
    if (kind != NULL) {
        *kind = payload[8];
    }
    if (data_len != NULL) {
        *data_len = 0U;
        if (payload[8] == 2U && payload_len >= 11U) {
            *data_len = read_be16_runtime(payload + 9U);
        } else if (payload[8] == 5U && payload_len >= 13U) {
            uint16_t addr_len = read_be16_runtime(payload + 9U);
            if (payload_len >= 13U + addr_len) {
                *data_len = read_be16_runtime(payload + 11U + addr_len);
            }
        }
    }
    return 1;
}

static int split_host_port(const char *address, char *host, size_t host_len, uint16_t *port) {
    const char *colon = strrchr(address, ':');
    unsigned long parsed_port;
    char *end = NULL;
    if (colon == NULL || colon == address) {
        return 0;
    }
    snprintf(host, host_len, "%.*s", (int) (colon - address), address);
    parsed_port = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || parsed_port > 65535UL) {
        return 0;
    }
    *port = (uint16_t) parsed_port;
    return 1;
}

static int runtime_endpoint_addr(const xgw_node_hops_t *hops,
                                 const xgw_endpoint_t *endpoint,
                                 char *host,
                                 size_t host_len,
                                 uint16_t *port) {
    char address[XGW_MAX_NAME_LEN];
    if (host != NULL && host_len > 0U) {
        host[0] = '\0';
    }
    if (port != NULL) {
        *port = 0U;
    }
    if (endpoint == NULL) {
        return 0;
    }
    if (!xgw_route_endpoint_address(hops, endpoint, address, sizeof(address))) {
        return 0;
    }
    return split_host_port(address, host, host_len, port);
}

static uint32_t classify_target_priority_runtime(const char *host, uint16_t port) {
    (void) host;
    if (port == 53U || port == 123U) {
        return XGW_FLOW_PRIORITY_INTERACTIVE;
    }
    if (port == 80U || port == 443U || port == 5223U) {
        return XGW_FLOW_PRIORITY_DEFAULT;
    }
    return XGW_FLOW_PRIORITY_BULK;
}

static xgw_bridge_target_entry_t *find_bridge_target_by_host_port(xgw_bridge_target_entry_t *entries,
                                                                  const char *host,
                                                                  uint16_t port) {
    size_t i;
    if (entries == NULL || host == NULL || host[0] == '\0' || port == 0U) {
        return NULL;
    }
    for (i = 0U; i < XGW_BRIDGE_TARGET_MAP_SIZE; ++i) {
        if (!entries[i].active) {
            continue;
        }
        if (entries[i].target_port == port &&
            strcmp(entries[i].target_host, host) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static void fill_flow_hint_from_target(const xgw_bridge_target_entry_t *entry, xgw_flow_hint_t *hint) {
    if (hint == NULL) {
        return;
    }
    memset(hint, 0, sizeof(*hint));
    if (entry == NULL) {
        return;
    }
    hint->valid = 1U;
    hint->flow_class = entry->flow_class;
    hint->priority = entry->priority;
    hint->budget_flags = entry->budget_flags;
    hint->preferred_copies = entry->preferred_copies;
    snprintf(hint->frontend, sizeof(hint->frontend), "%s", entry->frontend);
    snprintf(hint->session_id, sizeof(hint->session_id), "%s", entry->front_session_id);
    snprintf(hint->route_name, sizeof(hint->route_name), "%s", entry->route_name);
    snprintf(hint->line_id, sizeof(hint->line_id), "%s", entry->line_id);
    hint->read_timeout_ms = entry->read_timeout_ms;
    hint->idle_after_first_byte_ms = entry->idle_after_first_byte_ms;
}

static void fill_flow_hint_runtime_metrics(const xgw_stream_sched_entry_t *entry, xgw_flow_hint_t *hint) {
    uint64_t delay_us;
    if (entry == NULL || hint == NULL) {
        return;
    }
    hint->runtime_inflight_bytes = entry->inflight_bytes > 0xffffffffULL ? 0xffffffffU : (uint32_t) entry->inflight_bytes;
    hint->runtime_send_credit_bytes = entry->send_credit_bytes > 0xffffffffULL ? 0xffffffffU : (uint32_t) entry->send_credit_bytes;
    delay_us = entry->enqueued_us > 0U && monotonic_us() > entry->enqueued_us ? monotonic_us() - entry->enqueued_us : 0U;
    hint->runtime_return_delay_ms = delay_us / 1000ULL > 65535ULL ? 65535U : (uint16_t) (delay_us / 1000ULL);
    hint->runtime_ack_credit_frames = entry->ack_credit_frames > 255U ? 255U : (uint8_t) entry->ack_credit_frames;
    hint->runtime_deficit_bytes = entry->deficit_bytes > 0xffffffffULL ? 0xffffffffU : (uint32_t) entry->deficit_bytes;
    hint->runtime_starvation_boost = entry->starvation_rounds > 65535U ? 65535U : (uint16_t) entry->starvation_rounds;
    hint->runtime_parity_budget = entry->parity_budget;
    hint->runtime_feedback_cadence_ms = entry->feedback_cadence_ms;
    hint->runtime_loss_ppm = entry->rolling_loss_ppm;
    hint->runtime_recovered_ppm = entry->rolling_recovered_ppm;
}

static void sched_deactivate(xgw_stream_scheduler_t *sched, xgw_stream_sched_entry_t *entry) {
    size_t pos;
    size_t last_entry_idx;
    if (sched == NULL || entry == NULL || !entry->active) {
        return;
    }
    pos = entry->slot_pos;
    if (pos >= sched->active_n) {
        return;
    }
    /* swap-remove: overwrite this position with the last active entry */
    last_entry_idx = sched->active_idx[sched->active_n - 1U];
    sched->active_idx[pos] = last_entry_idx;
    sched->entries[last_entry_idx].slot_pos = pos;
    sched->active_n--;
    memset(entry, 0, sizeof(*entry));
}

static void sched_activate(xgw_stream_scheduler_t *sched, size_t idx, uint32_t stream_id) {
    xgw_stream_sched_entry_t *entry;
    if (sched == NULL || idx >= XGW_STREAM_SCHED_CAP) {
        return;
    }
    if (sched->active_n >= XGW_STREAM_SCHED_CAP) {
        return;
    }
    entry = &sched->entries[idx];
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->stream_id = stream_id;
    entry->slot_pos = sched->active_n;
    sched->active_idx[sched->active_n] = idx;
    sched->active_n++;
}

static xgw_stream_sched_entry_t *stream_sched_find(xgw_stream_scheduler_t *sched, uint32_t stream_id) {
    size_t i;
    if (sched == NULL) {
        return NULL;
    }
    for (i = 0U; i < sched->active_n; ++i) {
        size_t idx = sched->active_idx[i];
        if (sched->entries[idx].active && sched->entries[idx].stream_id == stream_id) {
            return &sched->entries[idx];
        }
    }
    return NULL;
}

static int stream_sched_entry_reclaimable(const xgw_stream_sched_entry_t *entry, uint64_t now_us) {
    uint64_t ref_us;
    if (entry == NULL || !entry->active) {
        return 0;
    }
    ref_us = entry->last_sent_us != 0U ? entry->last_sent_us : entry->enqueued_us;
    if (ref_us == 0U || now_us <= ref_us) {
        return 0;
    }
    if (entry->pending == 0U && entry->inflight_bytes == 0U && now_us - ref_us > 2000000ULL) {
        return 1;
    }
    if (entry->pending != 0U && entry->inflight_bytes == 0U && now_us - ref_us > 10000000ULL) {
        return 1;
    }
    return 0;
}

static void stream_sched_gc(xgw_stream_scheduler_t *sched, uint64_t now_us) {
    size_t k;
    if (sched == NULL) {
        return;
    }
    /* iterate active list; swap-remove may shift entries into the current position, so re-check k */
    for (k = 0U; k < sched->active_n; ) {
        size_t idx = sched->active_idx[k];
        xgw_stream_sched_entry_t *entry = &sched->entries[idx];
        if (stream_sched_entry_reclaimable(entry, now_us)) {
            sched_deactivate(sched, entry);
            continue;
        }
        k++;
    }
}

static int stream_sched_reclaim_one(xgw_stream_scheduler_t *sched, uint64_t now_us) {
    size_t k;
    size_t best_idx = XGW_STREAM_SCHED_CAP;
    uint8_t best_priority = 255U;
    uint64_t best_ref = 0U;
    if (sched == NULL) {
        return 0;
    }
    for (k = 0U; k < sched->active_n; ++k) {
        size_t idx = sched->active_idx[k];
        xgw_stream_sched_entry_t *entry = &sched->entries[idx];
        uint64_t ref_us;
        if (!entry->active) {
            continue;
        }
        ref_us = entry->last_sent_us != 0U ? entry->last_sent_us : entry->enqueued_us;
        if (stream_sched_entry_reclaimable(entry, now_us)) {
            sched_deactivate(sched, entry);
            return 1;
        }
        if (entry->pending != 0U || entry->inflight_bytes != 0U || ref_us == 0U || now_us <= ref_us) {
            continue;
        }
        if (now_us - ref_us < 250000ULL) {
            continue;
        }
        if (best_idx == XGW_STREAM_SCHED_CAP ||
            entry->hint.priority < best_priority ||
            (entry->hint.priority == best_priority && ref_us < best_ref)) {
            best_idx = idx;
            best_priority = entry->hint.priority;
            best_ref = ref_us;
        }
    }
    if (best_idx == XGW_STREAM_SCHED_CAP) {
        return 0;
    }
    sched_deactivate(sched, &sched->entries[best_idx]);
    return 1;
}

static size_t stream_sched_active_count(const xgw_stream_scheduler_t *sched) {
    if (sched == NULL) {
        return 0U;
    }
    return sched->active_n;
}

static xgw_stream_sched_entry_t *stream_sched_alloc(xgw_stream_scheduler_t *sched, uint32_t stream_id) {
    size_t i;
    xgw_stream_sched_entry_t *entry = stream_sched_find(sched, stream_id);
    if (entry != NULL) {
        return entry;
    }
    if (sched == NULL) {
        return NULL;
    }
    for (i = 0U; i < XGW_STREAM_SCHED_CAP; ++i) {
        if (!sched->entries[i].active) {
            sched_activate(sched, i, stream_id);
            return &sched->entries[i];
        }
    }
    if (stream_sched_reclaim_one(sched, monotonic_us())) {
        for (i = 0U; i < XGW_STREAM_SCHED_CAP; ++i) {
            if (!sched->entries[i].active) {
                sched_activate(sched, i, stream_id);
                return &sched->entries[i];
            }
        }
    }
    return NULL;
}

static int parse_stream_id_from_bridge_payload(const uint8_t *payload, size_t payload_len, uint32_t *stream_id) {
    return parse_bridge_payload_meta(payload, payload_len, stream_id, NULL, NULL);
}

static int stream_sched_enqueue(xgw_stream_scheduler_t *sched,
                                const uint8_t *payload,
                                size_t payload_len,
                                const char *line_id,
                                xgw_channel_direction_t send_direction,
                                const xgw_bridge_target_entry_t *target) {
    uint32_t stream_id = 0U;
    xgw_stream_sched_entry_t *entry;
    if (sched == NULL || payload == NULL || payload_len == 0U || payload_len > XGW_STREAM_SCHED_PAYLOAD_MAX) {
        return 0;
    }
    if (!parse_stream_id_from_bridge_payload(payload, payload_len, &stream_id)) {
        return 0;
    }
    entry = stream_sched_alloc(sched, stream_id);
    if (entry == NULL) {
        return 0;
    }
    memcpy(entry->payload, payload, payload_len);
    entry->payload_len = payload_len;
    entry->pending = 1U;
    entry->enqueued_us = monotonic_us();
    entry->last_sent_us = entry->last_sent_us;
    if (entry->send_credit_bytes == 0U) {
        entry->send_credit_bytes = stream_sched_budget_bytes(entry);
    }
    if (entry->last_credit_refill_us == 0U) {
        entry->last_credit_refill_us = entry->enqueued_us;
    }
    if (entry->feedback_cadence_ms == 0U) {
        if (entry->hint.valid && entry->hint.flow_class == 8U) {
            entry->feedback_cadence_ms = 10U;
        } else {
            entry->feedback_cadence_ms = entry->hint.valid && entry->hint.priority >= XGW_FLOW_PRIORITY_INTERACTIVE ? 8U : 25U;
        }
    }
    if (entry->parity_budget == 0U) {
        if (entry->hint.valid && (entry->hint.budget_flags & XGW_FLOW_BUDGET_REDUCED_FEC) != 0U) {
            entry->parity_budget = 1U;
        } else if (entry->hint.valid && entry->hint.flow_class == 8U) {
            entry->parity_budget = 2U;
        } else if (entry->hint.valid && entry->hint.priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
            entry->parity_budget = 2U;
        }
    }
    snprintf(entry->line_id, sizeof(entry->line_id), "%s", line_id == NULL ? "" : line_id);
    entry->send_direction = send_direction;
    if (target != NULL) {
        snprintf(entry->target_host, sizeof(entry->target_host), "%s", target->target_host);
        entry->target_port = target->target_port;
        fill_flow_hint_from_target(target, &entry->hint);
        snprintf(entry->frontend, sizeof(entry->frontend), "%s", target->frontend);
        snprintf(entry->front_session_id, sizeof(entry->front_session_id), "%s", target->front_session_id);
        snprintf(entry->route_name, sizeof(entry->route_name), "%s", target->route_name);
        if (!entry->hint.valid) {
            entry->hint.valid = 1U;
        }
        if (entry->hint.priority == 0U) {
            entry->hint.priority = classify_target_priority_runtime(target->target_host, target->target_port);
        }
    } else {
        memset(&entry->hint, 0, sizeof(entry->hint));
        snprintf(entry->target_host, sizeof(entry->target_host), "%s", "");
        entry->target_port = 0U;
    }
    if (entry->hint.priority == 0U) {
        entry->hint.priority = XGW_FLOW_PRIORITY_DEFAULT;
    }
    entry->session_slot = xgw_flow_class_to_slot(entry->hint.flow_class);
    if (entry->payload_len <= 256U && entry->hint.priority < XGW_FLOW_PRIORITY_INTERACTIVE) {
        entry->hint.priority = XGW_FLOW_PRIORITY_INTERACTIVE;
    }
    return 1;
}

static uint32_t stream_sched_behavior_priority(const xgw_stream_sched_entry_t *entry, uint64_t now_us) {
    uint32_t pri;
    uint64_t wait_us = 0U;
    if (entry == NULL) {
        return XGW_FLOW_PRIORITY_BULK;
    }
    pri = entry->hint.valid ? entry->hint.priority : XGW_FLOW_PRIORITY_DEFAULT;
    if (entry->hint.valid && entry->hint.flow_class == 2U && pri < XGW_FLOW_PRIORITY_CRITICAL) {
        pri = XGW_FLOW_PRIORITY_CRITICAL;
    }
    if (entry->hint.valid &&
        (entry->hint.flow_class == 3U || entry->hint.flow_class == 8U) &&
        pri < XGW_FLOW_PRIORITY_INTERACTIVE) {
        pri = XGW_FLOW_PRIORITY_INTERACTIVE;
    }
    if (entry->first_return_us == 0U) {
        if (entry->payload_len <= 1536U && pri < XGW_FLOW_PRIORITY_INTERACTIVE) {
            pri = XGW_FLOW_PRIORITY_INTERACTIVE;
        }
        if (entry->enqueued_us > 0U && now_us > entry->enqueued_us) {
            wait_us = now_us - entry->enqueued_us;
        }
        if (wait_us > 300000ULL && pri < XGW_FLOW_PRIORITY_CRITICAL) {
            pri++;
        }
        return pri;
    }
    if (entry->sent_bytes_total > 8192ULL &&
        entry->delivered_return_bytes < 4096ULL &&
        pri > XGW_FLOW_PRIORITY_DEFAULT) {
        pri--;
    }
    if (entry->ack_credit_frames == 0U &&
        entry->payload_len <= 512U &&
        pri < XGW_FLOW_PRIORITY_INTERACTIVE) {
        pri = XGW_FLOW_PRIORITY_INTERACTIVE;
    }
    return pri;
}

static xgw_stream_sched_entry_t *stream_sched_pick(xgw_stream_scheduler_t *sched,
                                                   const xgw_runtime_channels_t *channels) {
    size_t best_idx = XGW_STREAM_SCHED_CAP;
    uint32_t best_priority = 0U;
    uint64_t best_age = 0U;
    uint64_t now_us = monotonic_us();
    size_t start;
    size_t k;
    if (sched == NULL || sched->active_n == 0U) {
        return NULL;
    }

    /* O(N) 预聚合一次，循环内 total_weight/group_* 改为 O(1) 查表；返回前置 valid=0
     * 使非 pick 路径仍走 _ref。单次 pick 内 entry 的分组/inflight/pending 不变，聚合恒有效。 */
    stream_sched_agg_rebuild(sched);

    if (sched->rr_cursor >= sched->active_n) {
        sched->rr_cursor = 0U;
    }
    start = (sched->rr_cursor + 1U) % sched->active_n;
    for (k = 0U; k < sched->active_n; ++k) {
        size_t idx = sched->active_idx[(start + k) % sched->active_n];
            xgw_stream_sched_entry_t *entry = &sched->entries[idx];
            uint32_t pri;
            uint64_t age;
            if (!entry->active || entry->pending == 0U) {
                continue;
            }
        {
            xgw_session_t *session = NULL;
            uint64_t quantum = 8ULL * 1024ULL;
            if (channels != NULL) {
                session = runtime_channel_session_for_sched((xgw_runtime_channels_t *) channels,
                                                            entry->line_id,
                                                            entry->send_direction,
                                                            entry->session_slot,
                                                            0);
            }
            stream_sched_refill_credit(sched, entry, session, now_us);
            quantum = stream_sched_session_refill_rate_bytes_per_sec(sched, entry, session) / 20ULL;
            if (quantum < 8ULL * 1024ULL) {
                quantum = 8ULL * 1024ULL;
            }
            entry->deficit_bytes += quantum;
            entry->starvation_rounds++;
            /* 第一性：stream_sched 是纯调度器/整形器，拥塞控制由 session->cc(BBR) 在
             * send_frame_series_udp 层用 xgw_cc_can_send 闭环负责。这里只保留两道与 ACK
             * 无关、不会卡死的整形门控：
             *   - send_credit：时间驱动 pacing（refill 按 cc.pacing_rate 持续补充）。
             *   - deficit：DRR 公平配额，防单流独占。
             * 已移除原有 inflight_bytes/ack_credit_frames 影子拥塞窗口门控——它们没接隧道
             * ACK 反馈（mark_return 仅 ingress 调用），在 egress/relay 上只增不减导致回程停发
             * 30s。inflight/ack_credit 字段保留作遥测（dataplane FEC/ACK、session ACK cadence
             * 仍消费），但不再参与调度门控。 */
            if (entry->send_credit_bytes < entry->payload_len) {
                continue;
            }
            if (entry->deficit_bytes < entry->payload_len) {
                continue;
            }
        }
        pri = stream_sched_behavior_priority(entry, now_us);
        age = entry->enqueued_us > 0U && now_us > entry->enqueued_us ? now_us - entry->enqueued_us : 0U;
        if (best_idx == XGW_STREAM_SCHED_CAP ||
            (pri + (entry->starvation_rounds > 4U ? 1U : 0U)) > best_priority ||
            (pri == best_priority && age > best_age)) {
            best_idx = idx;
            best_priority = pri + (entry->starvation_rounds > 4U ? 1U : 0U);
            best_age = age;
        }
    }
    if (best_idx == XGW_STREAM_SCHED_CAP) {
        sched->agg.valid = 0;
        return NULL;
    }
    {
        xgw_stream_sched_entry_t *picked = &sched->entries[best_idx];
        uint64_t gap_ms = 0U;
        if (picked->last_return_us > 0U && now_us > picked->last_return_us) {
            gap_ms = (now_us - picked->last_return_us) / 1000ULL;
        } else if (picked->enqueued_us > 0U && now_us > picked->enqueued_us) {
            gap_ms = (now_us - picked->enqueued_us) / 1000ULL;
        }
        if (XGW_RT_VERBOSE()) printf("runtime.stream_sched.pick stream=%u target=%s:%u frontend=%s front_session_id=%s route=%s line=%s dir=%d slot=%s priority=%u behavior_priority=%u inflight=%llu send_credit=%llu ack_debt=%u parity_budget=%u gap_ms=%llu payload=%zu first_return=%d sent_total=%llu returned_total=%llu\n",
               picked->stream_id,
               picked->target_host,
               picked->target_port,
               picked->frontend,
               picked->front_session_id,
               picked->route_name,
               picked->line_id,
               (int) picked->send_direction,
               runtime_session_slot_name(picked->session_slot),
               picked->hint.priority,
               stream_sched_behavior_priority(picked, now_us),
               (unsigned long long) picked->inflight_bytes,
               (unsigned long long) picked->send_credit_bytes,
               picked->ack_credit_frames,
               picked->parity_budget,
               (unsigned long long) gap_ms,
               picked->payload_len,
               picked->first_return_us != 0U ? 1 : 0,
               (unsigned long long) picked->sent_bytes_total,
               (unsigned long long) picked->delivered_return_bytes);
        fflush(stdout);
    }
    sched->rr_cursor = (start + k) % sched->active_n;
    sched->agg.valid = 0;
    return &sched->entries[best_idx];
}

static void stream_sched_mark_sent(xgw_stream_sched_entry_t *entry) {
    if (entry == NULL) {
        return;
    }
    entry->pending = 0U;
    entry->last_sent_us = monotonic_us();
    if (entry->send_credit_bytes >= entry->payload_len) {
        entry->send_credit_bytes -= entry->payload_len;
    } else {
        entry->send_credit_bytes = 0U;
    }
    if (entry->deficit_bytes >= entry->payload_len) {
        entry->deficit_bytes -= entry->payload_len;
    } else {
        entry->deficit_bytes = 0U;
    }
    entry->sent_bytes_total += entry->payload_len;
    /* 遥测语义（非门控）：inflight/ack_credit 仍喂 hint 供 dataplane FEC/ACK 与 session ACK
     * cadence 消费；拥塞窗口由 session->cc(BBR) 负责，stream_sched 不再以此门控。 */
    entry->inflight_bytes += entry->payload_len;
    entry->ack_credit_frames++;
    entry->starvation_rounds = 0U;
    if (entry->rolling_loss_ppm > 0U) {
        entry->rolling_loss_ppm = (entry->rolling_loss_ppm * 7U) / 8U;
    }
    if (entry->hint.valid && entry->hint.priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
        if (entry->ack_credit_frames > 4U) {
            entry->feedback_cadence_ms = 4U;
        } else if (entry->ack_credit_frames > 2U) {
            entry->feedback_cadence_ms = 6U;
        } else {
            entry->feedback_cadence_ms = 8U;
        }
    } else {
        if (entry->ack_credit_frames > 6U) {
            entry->feedback_cadence_ms = 6U;
        } else if (entry->ack_credit_frames > 3U) {
            entry->feedback_cadence_ms = 10U;
        } else {
            entry->feedback_cadence_ms = 25U;
        }
    }
}

static void stream_sched_mark_return(xgw_stream_scheduler_t *sched, uint32_t stream_id, size_t bytes) {
    xgw_stream_sched_entry_t *entry = stream_sched_find(sched, stream_id);
    uint64_t now_us = monotonic_us();
    if (entry == NULL) {
        return;
    }
    if (entry->inflight_bytes >= bytes) {
        entry->inflight_bytes -= bytes;
    } else {
        entry->inflight_bytes = 0U;
    }
    entry->delivered_return_bytes += bytes;
    if (entry->first_return_us == 0U) {
        entry->first_return_us = now_us;
    }
    entry->last_return_us = now_us;
    if (entry->ack_credit_frames > 0U) {
        entry->ack_credit_frames--;
    }
    entry->send_credit_bytes += bytes;
    if (entry->rolling_recovered_ppm < 900000U) {
        entry->rolling_recovered_ppm = (entry->rolling_recovered_ppm * 7U + 1000000U) / 8U;
    }
    entry->rolling_loss_ppm = (entry->rolling_loss_ppm * 7U) / 8U;
    if (entry->ack_credit_frames == 0U) {
        if (entry->hint.valid && entry->hint.priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
            entry->feedback_cadence_ms = 8U;
        } else if (entry->hint.valid && entry->hint.flow_class == 8U) {
            entry->feedback_cadence_ms = 10U;
        } else {
            entry->feedback_cadence_ms = 25U;
        }
    }
    if (entry->parity_budget > 0U &&
        entry->rolling_recovered_ppm > 800000U &&
        entry->rolling_loss_ppm < 120000U &&
        entry->ack_credit_frames == 0U) {
        entry->parity_budget--;
    }
    if (entry->parity_budget == 0U &&
        entry->rolling_loss_ppm > 250000U &&
        entry->hint.valid &&
        (entry->hint.budget_flags & XGW_FLOW_BUDGET_REDUCED_FEC) == 0U) {
        if (entry->hint.flow_class == 8U) {
            entry->parity_budget = 2U;
        } else {
            entry->parity_budget = entry->hint.priority >= XGW_FLOW_PRIORITY_INTERACTIVE ? 2U : 1U;
        }
    }
}

static uint64_t stream_sched_budget_bytes(const xgw_stream_sched_entry_t *entry) {
    if (entry == NULL) {
        return 256ULL * 1024ULL;
    }
    if (entry->hint.valid && entry->hint.flow_class == 2U) {
        return 512ULL * 1024ULL;
    }
    if (entry->hint.valid && entry->hint.flow_class == 3U) {
        return 8ULL * 1024ULL * 1024ULL;
    }
    if (entry->hint.valid && entry->hint.flow_class == 8U) {
        return 2ULL * 1024ULL * 1024ULL;
    }
    if (entry->hint.valid && entry->hint.priority >= XGW_FLOW_PRIORITY_CRITICAL) {
        return 2ULL * 1024ULL * 1024ULL;
    }
    if (entry->hint.valid && entry->hint.priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
        return 1ULL * 1024ULL * 1024ULL;
    }
    return 512ULL * 1024ULL;
}

static uint32_t stream_sched_priority_weight(uint8_t priority) {
    if (priority >= XGW_FLOW_PRIORITY_CRITICAL) {
        return 4U;
    }
    if (priority >= XGW_FLOW_PRIORITY_INTERACTIVE) {
        return 2U;
    }
    return 1U;
}

static uint32_t stream_sched_flow_weight(const xgw_stream_sched_entry_t *entry) {
    if (entry == NULL || !entry->hint.valid) {
        return 1U;
    }
    if (entry->hint.flow_class == 2U) {
        return 5U;
    }
    if (entry->hint.flow_class == 3U) {
        return 3U;
    }
    if (entry->hint.flow_class == 8U) {
        return 2U;
    }
    return 1U;
}

/* ===== O(N) 调度聚合实现：FNV64 + 代际哈希表，容斥统计分组量。
 * 旧 O(N^2) 扫描保留为 *_ref，仅在 agg.valid==0 时回退，并由 selftest 对拍。===== */
static uint64_t sched_fnv(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s != '\0') { h ^= (unsigned char) *s++; h *= 1099511628211ULL; }
    return h;
}

static uint64_t sched_mix(uint64_t h, uint32_t tag) {
    h ^= (uint64_t) tag * 0x9E3779B97F4A7C15ULL;
    h *= 1099511628211ULL;
    h ^= h >> 29;
    return h;
}

static uint64_t sched_combine(uint64_t a, uint64_t b) {
    uint64_t h = a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2));
    return h ? h : 1ULL;
}

static sched_agg_slot_t *sched_map_upsert(sched_agg_map_t *m, uint64_t key, uint32_t gen) {
    uint64_t base = key & (SCHED_AGG_SLOTS - 1U);
    size_t i;
    for (i = 0U; i < SCHED_AGG_SLOTS; ++i) {
        sched_agg_slot_t *s = &m->slots[(base + i) & (SCHED_AGG_SLOTS - 1U)];
        if (s->gen != gen) {
            s->gen = gen; s->key = key; s->cnt = 0U; s->sum = 0U; s->wsum = 0U;
            return s;
        }
        if (s->key == key) return s;
    }
    return NULL;
}

static const sched_agg_slot_t *sched_map_find(const sched_agg_map_t *m, uint64_t key, uint32_t gen) {
    uint64_t base = key & (SCHED_AGG_SLOTS - 1U);
    size_t i;
    for (i = 0U; i < SCHED_AGG_SLOTS; ++i) {
        const sched_agg_slot_t *s = &m->slots[(base + i) & (SCHED_AGG_SLOTS - 1U)];
        if (s->gen != gen) return NULL;
        if (s->key == key) return s;
    }
    return NULL;
}

static int stream_sched_same_route_group(const xgw_stream_sched_entry_t *entry,
                                         const xgw_stream_sched_entry_t *other) {
    if (entry == NULL || other == NULL) {
        return 0;
    }
    if (entry->route_name[0] != '\0' && other->route_name[0] != '\0' &&
        strcmp(entry->route_name, other->route_name) == 0 &&
        entry->send_direction == other->send_direction &&
        entry->session_slot == other->session_slot) {
        return 1;
    }
    if (entry->line_id[0] != '\0' && other->line_id[0] != '\0' &&
        strcmp(entry->line_id, other->line_id) == 0 &&
        entry->send_direction == other->send_direction &&
        entry->session_slot == other->session_slot) {
        return 1;
    }
    return 0;
}

static int stream_sched_same_session_group(const xgw_stream_sched_entry_t *entry,
                                           const xgw_stream_sched_entry_t *other) {
    if (entry == NULL || other == NULL) {
        return 0;
    }
    return entry->front_session_id[0] != '\0' &&
           other->front_session_id[0] != '\0' &&
           strcmp(entry->front_session_id, other->front_session_id) == 0;
}

static uint32_t stream_sched_group_pending_count_ref(const xgw_stream_scheduler_t *sched,
                                                 const xgw_stream_sched_entry_t *pivot) {
    size_t k;
    uint32_t count = 0U;
    if (sched == NULL || pivot == NULL) {
        return 0U;
    }
    for (k = 0U; k < sched->active_n; ++k) {
        const xgw_stream_sched_entry_t *entry = &sched->entries[sched->active_idx[k]];
        if (!entry->active || entry->pending == 0U) {
            continue;
        }
        if (stream_sched_same_session_group(entry, pivot) || stream_sched_same_route_group(entry, pivot)) {
            count++;
        }
    }
    return count;
}

static int stream_sched_same_path(const xgw_stream_sched_entry_t *entry,
                                  const char *line_id,
                                  xgw_channel_direction_t direction) {
    if (entry == NULL || line_id == NULL || line_id[0] == '\0' || direction == XGW_CHANNEL_NONE) {
        return 0;
    }
    return strcmp(entry->line_id, line_id) == 0 && entry->send_direction == direction;
}

static uint32_t stream_sched_total_weight_ref(const xgw_stream_scheduler_t *sched,
                                          const char *line_id,
                                          xgw_channel_direction_t direction) {
    size_t k;
    uint32_t total = 0U;
    if (sched == NULL) {
        return 1U;
    }
    for (k = 0U; k < sched->active_n; ++k) {
        const xgw_stream_sched_entry_t *entry = &sched->entries[sched->active_idx[k]];
        if (!entry->active) {
            continue;
        }
        if (!stream_sched_same_path(entry, line_id, direction)) {
            continue;
        }
        if (!entry->pending && entry->inflight_bytes == 0U) {
            continue;
        }
        total += stream_sched_priority_weight(entry->hint.priority) * stream_sched_flow_weight(entry);
    }
    return total == 0U ? 1U : total;
}

typedef struct sched_keys {
    int has_s, has_r, has_l;
    uint64_t ks, kr, kl;
} sched_keys_t;

/* 同一条 entry 在 rebuild 与 query 用同一套 key，保证分桶一致。
 * R/L 含 (dir,slot)；S 仅 fsid；与 _ref 的 same_*_group 谓词逐字段对应。 */
static sched_keys_t sched_entry_keys(const xgw_stream_sched_entry_t *e) {
    sched_keys_t k;
    uint32_t tag = (uint32_t) ((unsigned) e->send_direction * 3U + (unsigned) e->session_slot + 1U);
    k.has_s = e->front_session_id[0] != '\0';
    k.has_r = e->route_name[0] != '\0';
    k.has_l = e->line_id[0] != '\0';
    k.ks = k.has_s ? sched_fnv(e->front_session_id) : 0U;
    k.kr = k.has_r ? sched_mix(sched_fnv(e->route_name), tag) : 0U;
    k.kl = k.has_l ? sched_mix(sched_fnv(e->line_id), tag) : 0U;
    return k;
}

static void sched_agg_add(sched_agg_map_t *m, uint64_t key, uint32_t gen, int pending, uint64_t inflight) {
    sched_agg_slot_t *s = sched_map_upsert(m, key, gen);
    if (s == NULL) return;
    if (pending) s->cnt += 1U;
    s->sum += inflight;
}

static void stream_sched_agg_rebuild(xgw_stream_scheduler_t *sched) {
    stream_sched_agg_t *a;
    size_t k;
    if (sched == NULL) return;
    a = &sched->agg;
    a->gen += 1U;
    if (a->gen == 0U) { /* 代际回绕：清表后从 1 起，确保无残留命中 */
        memset(&a->s, 0, sizeof(a->s)); memset(&a->r, 0, sizeof(a->r));
        memset(&a->l, 0, sizeof(a->l)); memset(&a->sr, 0, sizeof(a->sr));
        memset(&a->sl, 0, sizeof(a->sl)); memset(&a->rl, 0, sizeof(a->rl));
        memset(&a->srl, 0, sizeof(a->srl)); memset(&a->w, 0, sizeof(a->w));
        a->gen = 1U;
    }
    for (k = 0U; k < sched->active_n; ++k) {
        const xgw_stream_sched_entry_t *e = &sched->entries[sched->active_idx[k]];
        sched_keys_t kk;
        int pend;
        if (!e->active) continue;
        kk = sched_entry_keys(e);
        pend = e->pending != 0U;
        if (kk.has_s) sched_agg_add(&a->s, kk.ks, a->gen, pend, e->inflight_bytes);
        if (kk.has_r) sched_agg_add(&a->r, kk.kr, a->gen, pend, e->inflight_bytes);
        if (kk.has_l) sched_agg_add(&a->l, kk.kl, a->gen, pend, e->inflight_bytes);
        if (kk.has_s && kk.has_r) sched_agg_add(&a->sr, sched_combine(kk.ks, kk.kr), a->gen, pend, e->inflight_bytes);
        if (kk.has_s && kk.has_l) sched_agg_add(&a->sl, sched_combine(kk.ks, kk.kl), a->gen, pend, e->inflight_bytes);
        if (kk.has_r && kk.has_l) sched_agg_add(&a->rl, sched_combine(kk.kr, kk.kl), a->gen, pend, e->inflight_bytes);
        if (kk.has_s && kk.has_r && kk.has_l)
            sched_agg_add(&a->srl, sched_combine(sched_combine(kk.ks, kk.kr), kk.kl), a->gen, pend, e->inflight_bytes);
        /* W 桶：与 total_weight_ref 同口径——仅 line 非空、dir!=NONE、(pending||inflight)，键不含 slot */
        if (kk.has_l && e->send_direction != XGW_CHANNEL_NONE && (pend || e->inflight_bytes != 0U)) {
            uint64_t wkey = sched_mix(sched_fnv(e->line_id), (uint32_t) e->send_direction + 1U);
            sched_agg_slot_t *ws = sched_map_upsert(&a->w, wkey, a->gen);
            if (ws != NULL) ws->wsum += stream_sched_priority_weight(e->hint.priority) * stream_sched_flow_weight(e);
        }
    }
    a->valid = 1;
}

/* 容斥求 |S∪R∪L| 的 (pending数, inflight和)：S=同fsid, R=同route+dir+slot, L=同line+dir+slot。 */
static sched_group_acc_t stream_sched_group_query(const xgw_stream_scheduler_t *sched,
                                                  const xgw_stream_sched_entry_t *pivot) {
    const stream_sched_agg_t *a = &sched->agg;
    sched_keys_t k = sched_entry_keys(pivot);
    int64_t c = 0, s = 0;
    const sched_agg_slot_t *p;
#define SCHED_Q(MAP, COND, KEY, SGN) do { \
        if (COND) { p = sched_map_find(&a->MAP, (KEY), a->gen); \
            if (p != NULL) { c += (SGN) * (int64_t) p->cnt; s += (SGN) * (int64_t) p->sum; } } \
    } while (0)
    SCHED_Q(s,   k.has_s, k.ks, 1);
    SCHED_Q(r,   k.has_r, k.kr, 1);
    SCHED_Q(l,   k.has_l, k.kl, 1);
    SCHED_Q(sr,  k.has_s && k.has_r, sched_combine(k.ks, k.kr), -1);
    SCHED_Q(sl,  k.has_s && k.has_l, sched_combine(k.ks, k.kl), -1);
    SCHED_Q(rl,  k.has_r && k.has_l, sched_combine(k.kr, k.kl), -1);
    SCHED_Q(srl, k.has_s && k.has_r && k.has_l, sched_combine(sched_combine(k.ks, k.kr), k.kl), 1);
#undef SCHED_Q
    {
        sched_group_acc_t out;
        out.cnt = c < 0 ? 0U : (uint64_t) c;
        out.sum = s < 0 ? 0U : (uint64_t) s;
        return out;
    }
}

static uint32_t stream_sched_group_pending_count(const xgw_stream_scheduler_t *sched,
                                                 const xgw_stream_sched_entry_t *pivot) {
    if (sched == NULL || pivot == NULL) return 0U;
    if (!sched->agg.valid) return stream_sched_group_pending_count_ref(sched, pivot);
    return (uint32_t) stream_sched_group_query(sched, pivot).cnt;
}

static uint32_t stream_sched_total_weight(const xgw_stream_scheduler_t *sched,
                                          const char *line_id,
                                          xgw_channel_direction_t direction) {
    const sched_agg_slot_t *p;
    uint64_t wkey;
    if (sched == NULL) return 1U;
    if (!sched->agg.valid) return stream_sched_total_weight_ref(sched, line_id, direction);
    if (line_id == NULL || line_id[0] == '\0' || direction == XGW_CHANNEL_NONE) return 1U;
    wkey = sched_mix(sched_fnv(line_id), (uint32_t) direction + 1U);
    p = sched_map_find(&sched->agg.w, wkey, sched->agg.gen);
    if (p == NULL || p->wsum == 0U) return 1U;
    return p->wsum;
}

static uint64_t stream_sched_session_budget_bytes(const xgw_stream_scheduler_t *sched,
                                                  const xgw_stream_sched_entry_t *entry,
                                                  const xgw_session_t *session) {
    uint64_t cwnd = 128ULL * 1024ULL;
    uint32_t total_weight;
    uint32_t my_weight;
    uint64_t share;
    if (entry == NULL) {
        return 64ULL * 1024ULL;
    }
    if (session != NULL && session->cc.cwnd_bytes > 0U) {
        cwnd = session->cc.cwnd_bytes;
    }
    total_weight = stream_sched_total_weight(sched, entry->line_id, entry->send_direction);
    my_weight = stream_sched_priority_weight(entry->hint.priority) * stream_sched_flow_weight(entry);
    share = (cwnd * (uint64_t) my_weight) / (uint64_t) total_weight;
    if (stream_sched_group_pending_count(sched, entry) > 8U) {
        share = (share * 3ULL) / 2ULL;
    }
    if (share < 128ULL * 1024ULL) {
        share = 128ULL * 1024ULL;
    }
    return share;
}

static uint64_t stream_sched_session_refill_rate_bytes_per_sec(const xgw_stream_scheduler_t *sched,
                                                               const xgw_stream_sched_entry_t *entry,
                                                               const xgw_session_t *session) {
    uint64_t pacing_bps = 8ULL * 1024ULL * 1024ULL;
    uint32_t total_weight;
    uint32_t my_weight;
    uint64_t share;
    if (entry == NULL) {
        return 128ULL * 1024ULL;
    }
    if (session != NULL && session->cc.pacing_rate_bps > 0U) {
        pacing_bps = session->cc.pacing_rate_bps;
    }
    total_weight = stream_sched_total_weight(sched, entry->line_id, entry->send_direction);
    my_weight = stream_sched_priority_weight(entry->hint.priority) * stream_sched_flow_weight(entry);
    share = ((pacing_bps / 8ULL) * (uint64_t) my_weight) / (uint64_t) total_weight;
    if (stream_sched_group_pending_count(sched, entry) > 8U) {
        share = (share * 3ULL) / 2ULL;
    }
    if (share < 512ULL * 1024ULL) {
        share = 512ULL * 1024ULL;
    }
    return share;
}

static void stream_sched_refill_credit(const xgw_stream_scheduler_t *sched,
                                       xgw_stream_sched_entry_t *entry,
                                       const xgw_session_t *session,
                                       uint64_t now_us) {
    uint64_t elapsed_us;
    uint64_t refill_bytes;
    uint64_t cap;
    if (entry == NULL) {
        return;
    }
    cap = stream_sched_session_budget_bytes(sched, entry, session);
    if (entry->last_credit_refill_us == 0U) {
        entry->last_credit_refill_us = now_us;
        if (entry->send_credit_bytes == 0U) {
            entry->send_credit_bytes = cap;
        }
        return;
    }
    if (now_us <= entry->last_credit_refill_us) {
        return;
    }
    elapsed_us = now_us - entry->last_credit_refill_us;
    refill_bytes = (stream_sched_session_refill_rate_bytes_per_sec(sched, entry, session) * elapsed_us) / 1000000ULL;
    if (refill_bytes == 0U) {
        return;
    }
    entry->send_credit_bytes = max_u64(entry->send_credit_bytes + refill_bytes, entry->send_credit_bytes);
    if (entry->send_credit_bytes > cap) {
        entry->send_credit_bytes = cap;
    }
    entry->last_credit_refill_us = now_us;
}

static void stream_sched_tick(xgw_stream_scheduler_t *sched, uint64_t now_us) {
    size_t k;
    if (sched == NULL) {
        return;
    }
    for (k = 0U; k < sched->active_n; ++k) {
        xgw_stream_sched_entry_t *entry = &sched->entries[sched->active_idx[k]];
        if (!entry->active) {
            continue;
        }
        if (entry->pending != 0U && entry->last_sent_us > 0U && now_us > entry->last_sent_us + 300000ULL) {
            uint32_t loss_add = entry->hint.valid && entry->hint.priority >= XGW_FLOW_PRIORITY_INTERACTIVE ? 180000U : 120000U;
            entry->rolling_loss_ppm = (entry->rolling_loss_ppm * 7U + loss_add) / 8U;
            if (entry->parity_budget < XGW_MAX_FEC_PARITY) {
                entry->parity_budget++;
            }
            if (entry->feedback_cadence_ms > 4U) {
                entry->feedback_cadence_ms = 4U;
            }
        } else if (entry->rolling_loss_ppm > 0U) {
            entry->rolling_loss_ppm = (entry->rolling_loss_ppm * 15U) / 16U;
        }
        if (!entry->pending && entry->inflight_bytes == 0U && now_us > entry->last_sent_us + 3000000ULL) {
            if (entry->parity_budget > 0U && entry->rolling_loss_ppm < 50000U) {
                entry->parity_budget--;
            }
            if (entry->feedback_cadence_ms < 25U) {
                entry->feedback_cadence_ms++;
            }
        }
    }
}

static int endpoint_matches_packet(const xgw_endpoint_t *endpoint, const xgw_packet_t *packet) {
    char host[64];
    uint16_t port = 0U;
    if (endpoint == NULL || packet == NULL) {
        return 0;
    }
    if (endpoint->address[0] != '\0' && split_host_port(endpoint->address, host, sizeof(host), &port) &&
        strcmp(host, packet->remote_host) == 0 && port == packet->remote_port) {
        return 1;
    }
    if (endpoint->public_address[0] != '\0' && split_host_port(endpoint->public_address, host, sizeof(host), &port) &&
        strcmp(host, packet->remote_host) == 0 && port == packet->remote_port) {
        return 1;
    }
    if (endpoint->private_address[0] != '\0' && split_host_port(endpoint->private_address, host, sizeof(host), &port) &&
        strcmp(host, packet->remote_host) == 0 && port == packet->remote_port) {
        return 1;
    }
    return 0;
}

static int endpoint_matches_host_port(const xgw_endpoint_t *endpoint, const char *host, uint16_t port) {
    char endpoint_host[64];
    uint16_t endpoint_port = 0U;
    if (endpoint == NULL || host == NULL || host[0] == '\0' || port == 0U) {
        return 0;
    }
    if (endpoint->address[0] != '\0' && split_host_port(endpoint->address, endpoint_host, sizeof(endpoint_host), &endpoint_port) &&
        strcmp(endpoint_host, host) == 0 && endpoint_port == port) {
        return 1;
    }
    if (endpoint->public_address[0] != '\0' && split_host_port(endpoint->public_address, endpoint_host, sizeof(endpoint_host), &endpoint_port) &&
        strcmp(endpoint_host, host) == 0 && endpoint_port == port) {
        return 1;
    }
    if (endpoint->private_address[0] != '\0' && split_host_port(endpoint->private_address, endpoint_host, sizeof(endpoint_host), &endpoint_port) &&
        strcmp(endpoint_host, host) == 0 && endpoint_port == port) {
        return 1;
    }
    return 0;
}

static int is_bridge_connect_type(const xgw_runtime_config_t *config) {
    return config != NULL && strcmp(config->connect_type, "bridge") == 0;
}

static xgw_session_t *get_send_session(xgw_dataplane_t *dp,
                                       const xgw_runtime_config_t *config,
                                       const char *line_id,
                                       xgw_channel_direction_t direction,
                                       xgw_session_slot_t slot,
                                       const char *host,
                                       uint16_t port);

static const xgw_node_hops_t *runtime_hops(const xgw_line_runtime_t *line) {
    return line == NULL ? NULL : &line->hops;
}

static const char *runtime_line_id(const xgw_line_runtime_t *line) {
    return line == NULL || line->config == NULL ? "" : line->config->id;
}

static const xgw_line_runtime_t *runtime_line_for_peer(const xgw_route_manager_t *routes,
                                                       const xgw_line_runtime_t *active_line,
                                                       const char *host,
                                                       uint16_t port) {
    const xgw_line_runtime_t *line = xgw_route_find_by_peer(routes, host, port);
    return line == NULL ? active_line : line;
}

static void log_channel_state(const char *prefix,
                              const char *line_id,
                              const xgw_neighbor_channel_t *channel) {
    if (prefix == NULL || channel == NULL) {
        return;
    }
    if (!XGW_RT_VERBOSE()) {
        return;
    }
    printf("%s line=%s dir=%d peer=%s:%u gen=%llu state=%d pending_ptr=%p pending_state=%d active_ptr=%p active_state=%d ctrl_pending=%p/%d ctrl_active=%p/%d media_pending=%p/%d media_active=%p/%d bulk_pending=%p/%d bulk_active=%p/%d\n",
           prefix,
           line_id == NULL ? "" : line_id,
           (int) channel->direction,
           channel->peer_host,
           channel->peer_port,
           (unsigned long long) channel->generation,
           (int) channel->state,
           (void *) channel->pending_session,
           channel->pending_session == NULL ? -1 : (int) channel->pending_session->control_state,
           (void *) channel->active_session,
           channel->active_session == NULL ? -1 : (int) channel->active_session->control_state,
           (void *) channel->pending_slots[XGW_SESSION_SLOT_CONTROL],
           channel->pending_slots[XGW_SESSION_SLOT_CONTROL] == NULL ? -1 : (int) channel->pending_slots[XGW_SESSION_SLOT_CONTROL]->control_state,
           (void *) channel->active_slots[XGW_SESSION_SLOT_CONTROL],
           channel->active_slots[XGW_SESSION_SLOT_CONTROL] == NULL ? -1 : (int) channel->active_slots[XGW_SESSION_SLOT_CONTROL]->control_state,
           (void *) channel->pending_slots[XGW_SESSION_SLOT_MEDIA],
           channel->pending_slots[XGW_SESSION_SLOT_MEDIA] == NULL ? -1 : (int) channel->pending_slots[XGW_SESSION_SLOT_MEDIA]->control_state,
           (void *) channel->active_slots[XGW_SESSION_SLOT_MEDIA],
           channel->active_slots[XGW_SESSION_SLOT_MEDIA] == NULL ? -1 : (int) channel->active_slots[XGW_SESSION_SLOT_MEDIA]->control_state,
           (void *) channel->pending_slots[XGW_SESSION_SLOT_BULK],
           channel->pending_slots[XGW_SESSION_SLOT_BULK] == NULL ? -1 : (int) channel->pending_slots[XGW_SESSION_SLOT_BULK]->control_state,
           (void *) channel->active_slots[XGW_SESSION_SLOT_BULK],
           channel->active_slots[XGW_SESSION_SLOT_BULK] == NULL ? -1 : (int) channel->active_slots[XGW_SESSION_SLOT_BULK]->control_state);
    fflush(stdout);
}

static const char *runtime_session_slot_name(xgw_session_slot_t slot) {
    switch (slot) {
        case XGW_SESSION_SLOT_CONTROL:
            return "control";
        case XGW_SESSION_SLOT_MEDIA:
            return "media";
        case XGW_SESSION_SLOT_BULK:
            return "bulk";
        default:
            return "unknown";
    }
}

static const xgw_runtime_line_channels_t *runtime_line_channels_find_const(const xgw_runtime_channels_t *channels,
                                                                           const char *line_id) {
    if (channels == NULL || line_id == NULL || line_id[0] == '\0') {
        return NULL;
    }
    if (channels->active.valid && strcmp(channels->active.line_id, line_id) == 0) {
        return &channels->active;
    }
    if (channels->candidate.valid && strcmp(channels->candidate.line_id, line_id) == 0) {
        return &channels->candidate;
    }
    if (channels->draining.valid && strcmp(channels->draining.line_id, line_id) == 0) {
        return &channels->draining;
    }
    return NULL;
}

static void log_route_manager_start(const xgw_route_manager_t *manager) {
    size_t i;
    if (manager == NULL) {
        return;
    }
    for (i = 0; i < manager->line_count; ++i) {
        const xgw_line_runtime_t *line = &manager->lines[i];
        char current_addr[XGW_MAX_NAME_LEN] = "";
        char previous_addr[XGW_MAX_NAME_LEN] = "";
        char next_addr[XGW_MAX_NAME_LEN] = "";
        xgw_route_endpoint_address(&line->hops, line->hops.current, current_addr, sizeof(current_addr));
        xgw_route_endpoint_address(&line->hops, line->hops.previous, previous_addr, sizeof(previous_addr));
        xgw_route_endpoint_address(&line->hops, line->hops.next, next_addr, sizeof(next_addr));
        printf("route.line id=%s state=%s current=%s previous=%s next=%s\n",
               runtime_line_id(line),
               xgw_line_state_name(line->state),
               current_addr,
               previous_addr,
               next_addr);
    }
    fflush(stdout);
}

static void runtime_channel_bind_endpoint(xgw_channel_set_t *channels,
                                          const char *line_id,
                                          const xgw_node_hops_t *hops,
                                          xgw_channel_direction_t direction,
                                          const xgw_endpoint_t *endpoint,
                                          const xgw_neighbor_channel_t *old_channel) {
    char host[64];
    uint16_t port = 0U;
    uint64_t generation = 1U;
    xgw_session_t *session = NULL;
    int same_peer = 0;
    if (channels == NULL) {
        return;
    }
    if (endpoint == NULL || !runtime_endpoint_addr(hops, endpoint, host, sizeof(host), &port)) {
        xgw_channel_bind(channels, direction, "", 0U, NULL);
        return;
    }
    if (old_channel != NULL &&
        old_channel->direction == direction &&
        strcmp(old_channel->peer_host, host) == 0 &&
        old_channel->peer_port == port) {
        same_peer = 1;
        generation = old_channel->generation == 0U ? 1U : old_channel->generation;
        session = old_channel->pending_session;
    } else if (old_channel != NULL && old_channel->generation > 0U) {
        generation = old_channel->generation + 1U;
    }
    xgw_channel_bind(channels, direction, host, port, session);
    {
        xgw_neighbor_channel_t *channel = xgw_channel_get(channels, direction);
        if (channel != NULL) {
            channel->generation = generation;
            if (same_peer) {
                size_t slot_idx;
                for (slot_idx = 0U; slot_idx < (size_t) XGW_SESSION_SLOT_COUNT; ++slot_idx) {
                    channel->pending_slots[slot_idx] = old_channel->pending_slots[slot_idx];
                    channel->active_slots[slot_idx] = old_channel->active_slots[slot_idx];
                }
                channel->pending_session = old_channel->pending_session;
                channel->active_session = old_channel->active_session;
                channel->state = old_channel->state;
            }
            log_channel_state("channel.pending.bind", line_id, channel);
        }
    }
}

static void runtime_line_channels_refresh(xgw_runtime_line_channels_t *line_channels,
                                          const xgw_runtime_channels_t *previous,
                                          const xgw_line_runtime_t *line) {
    const xgw_runtime_line_channels_t *old_line = NULL;
    const xgw_neighbor_channel_t *old_prev = NULL;
    const xgw_neighbor_channel_t *old_next = NULL;
    if (line_channels == NULL) {
        return;
    }
    memset(line_channels, 0, sizeof(*line_channels));
    if (line == NULL) {
        return;
    }
    line_channels->valid = 1;
    snprintf(line_channels->line_id, sizeof(line_channels->line_id), "%s", runtime_line_id(line));
    if (XGW_RT_VERBOSE()) {
    printf("runtime.line.refresh line=%s state=%s\n",
           line_channels->line_id,
           xgw_line_state_name(line->state));
    fflush(stdout);
    }
    old_line = runtime_line_channels_find_const(previous, line_channels->line_id);
    if (old_line != NULL) {
        old_prev = xgw_channel_get((xgw_channel_set_t *) &old_line->channels, XGW_CHANNEL_PREVIOUS);
        old_next = xgw_channel_get((xgw_channel_set_t *) &old_line->channels, XGW_CHANNEL_NEXT);
    }
    xgw_channel_set_init(&line_channels->channels);
    runtime_channel_bind_endpoint(&line_channels->channels,
                                  line_channels->line_id,
                                  &line->hops,
                                  XGW_CHANNEL_PREVIOUS,
                                  line->hops.previous,
                                  old_prev);
    runtime_channel_bind_endpoint(&line_channels->channels,
                                  line_channels->line_id,
                                  &line->hops,
                                  XGW_CHANNEL_NEXT,
                                  line->hops.next,
                                  old_next);
}

static void runtime_channel_refresh(xgw_runtime_channels_t *channels,
                                    const xgw_route_manager_t *routes) {
    xgw_runtime_channels_t previous;
    if (channels == NULL) {
        return;
    }
    previous = *channels;
    memset(channels, 0, sizeof(*channels));
    if (routes == NULL) {
        return;
    }
    runtime_line_channels_refresh(&channels->active, &previous, xgw_route_active(routes));
    runtime_line_channels_refresh(&channels->candidate, &previous, xgw_route_candidate(routes));
    runtime_line_channels_refresh(&channels->draining, &previous, xgw_route_draining(routes));
    if (XGW_RT_VERBOSE()) {
    printf("runtime.channel.refresh active=%s candidate=%s draining=%s\n",
           channels->active.valid ? channels->active.line_id : "",
           channels->candidate.valid ? channels->candidate.line_id : "",
           channels->draining.valid ? channels->draining.line_id : "");
    fflush(stdout);
    }
}

static xgw_runtime_line_channels_t *runtime_line_channels_select(xgw_runtime_channels_t *channels, const char *line_id) {
    if (channels == NULL || line_id == NULL || line_id[0] == '\0') {
        return NULL;
    }
    if (channels->active.valid && strcmp(channels->active.line_id, line_id) == 0) {
        return &channels->active;
    }
    if (channels->candidate.valid && strcmp(channels->candidate.line_id, line_id) == 0) {
        return &channels->candidate;
    }
    if (channels->draining.valid && strcmp(channels->draining.line_id, line_id) == 0) {
        return &channels->draining;
    }
    return NULL;
}

static xgw_session_t *runtime_channel_session(xgw_runtime_channels_t *channels,
                                              const char *line_id,
                                              xgw_channel_direction_t direction,
                                              xgw_session_slot_t slot,
                                              int require_ready) {
    xgw_runtime_line_channels_t *line_channels;
    xgw_neighbor_channel_t *channel;
    xgw_session_t *session;
    xgw_session_t *bulk_ready = NULL;
    if (channels == NULL || direction == XGW_CHANNEL_NONE) {
        return NULL;
    }
    line_channels = runtime_line_channels_select(channels, line_id);
    if (line_channels == NULL) {
        printf("runtime.channel.select.miss want_line=%s dir=%d active=%s candidate=%s draining=%s\n",
               line_id == NULL ? "" : line_id,
               (int) direction,
               channels->active.valid ? channels->active.line_id : "",
               channels->candidate.valid ? channels->candidate.line_id : "",
               channels->draining.valid ? channels->draining.line_id : "");
        fflush(stdout);
        return NULL;
    }
    channel = xgw_channel_get(&line_channels->channels, direction);
    if (channel == NULL || channel->peer_host[0] == '\0' || channel->peer_port == 0U) {
        printf("runtime.channel.select.empty line=%s dir=%d\n",
               line_channels->line_id,
               (int) direction);
        fflush(stdout);
        return NULL;
    }
    session = xgw_channel_slot_session(channel, slot, 0);
    if (session == NULL) {
        /* 该 slot 完全没有 session（连非 ready 的都没有）。非 BULK slot 回退到 BULK 兜底通道，
         * 避免上层调度反复选中同一 entry 却发不出去而活锁空转（control/media slot 偶发为空时
         * 尤其常见）。BULK slot 由 active_slots[BULK]/active_session 双重兜底，几乎总可用。 */
        if (slot != XGW_SESSION_SLOT_BULK) {
            xgw_session_t *bulk = xgw_channel_slot_session(channel, XGW_SESSION_SLOT_BULK, require_ready);
            if (bulk != NULL) {
                return bulk;
            }
        }
        {
            /* 日志按 500ms 节流：no_active 在活锁/瞬态下会高频刷屏，fflush 本身偷 CPU。 */
            static uint64_t last_no_active_log_us = 0U;
            uint64_t now_log_us = monotonic_us();
            if (last_no_active_log_us == 0U || now_log_us - last_no_active_log_us >= 500000ULL) {
                last_no_active_log_us = now_log_us;
                printf("runtime.channel.select.no_active line=%s dir=%d slot=%s\n",
                       line_channels->line_id,
                       (int) direction,
                       runtime_session_slot_name(slot));
                log_channel_state("runtime.channel.select.no_active", line_channels->line_id, channel);
                fflush(stdout);
            }
        }
        return NULL;
    }
    if (require_ready &&
        xgw_channel_slot_session(channel, slot, 1) == NULL) {
        if (slot != XGW_SESSION_SLOT_BULK) {
            bulk_ready = xgw_channel_slot_session(channel, XGW_SESSION_SLOT_BULK, 1);
            if (bulk_ready != NULL) {
                printf("runtime.channel.select.fallback_bulk line=%s dir=%d slot=%s fallback_slot=%s session_state=%d\n",
                       line_channels->line_id,
                       (int) direction,
                       runtime_session_slot_name(slot),
                       runtime_session_slot_name(XGW_SESSION_SLOT_BULK),
                       (int) bulk_ready->control_state);
                fflush(stdout);
                log_channel_state("runtime.channel.select.fallback_bulk", line_channels->line_id, channel);
                return bulk_ready;
            }
        }
        printf("runtime.channel.select.not_ready_slot line=%s dir=%d slot=%s session_state=%d\n",
               line_channels->line_id,
               (int) direction,
               runtime_session_slot_name(slot),
               (int) session->control_state);
        fflush(stdout);
        log_channel_state("runtime.channel.select.not_ready", line_channels->line_id, channel);
        return NULL;
    }
    return session;
}

static xgw_session_t *runtime_channel_session_for_sched(xgw_runtime_channels_t *channels,
                                                        const char *line_id,
                                                        xgw_channel_direction_t direction,
                                                        xgw_session_slot_t slot,
                                                        int require_ready) {
    return runtime_channel_session(channels, line_id, direction, slot, require_ready);
}

static int runtime_line_channel_ready(const xgw_runtime_channels_t *channels,
                                      const char *line_id,
                                      xgw_channel_direction_t direction) {
    return runtime_channel_session((xgw_runtime_channels_t *) channels, line_id, direction, XGW_SESSION_SLOT_BULK, 1) != NULL;
}

static void runtime_channel_promote_established(const char *line_id, xgw_neighbor_channel_t *channel) {
    size_t slot;
    int promoted = 0;
    if (channel == NULL) {
        return;
    }
    for (slot = 0U; slot < (size_t) XGW_SESSION_SLOT_COUNT; ++slot) {
        xgw_channel_promote_slot(channel, (xgw_session_slot_t) slot);
        if (channel->active_slots[slot] != NULL) {
            promoted = 1;
        }
    }
    if (promoted) {
        channel->state = XGW_CHANNEL_STATE_ESTABLISHED;
        log_channel_state("channel.promote", line_id, channel);
    }
}

static void runtime_channels_promote_peer(xgw_runtime_channels_t *channels,
                                          const char *peer_host,
                                          uint16_t peer_port,
                                          xgw_session_t *session) {
    xgw_runtime_line_channels_t *sets[3];
    size_t i;
    if (channels == NULL || session == NULL || peer_host == NULL || peer_host[0] == '\0' || peer_port == 0U) {
        return;
    }
    sets[0] = &channels->active;
    sets[1] = &channels->candidate;
    sets[2] = &channels->draining;
    for (i = 0U; i < 3U; ++i) {
        xgw_runtime_line_channels_t *line_channels = sets[i];
        xgw_neighbor_channel_t *previous;
        xgw_neighbor_channel_t *next;
        size_t slot_idx;
        if (line_channels == NULL || !line_channels->valid) {
            continue;
        }
        previous = xgw_channel_get(&line_channels->channels, XGW_CHANNEL_PREVIOUS);
        next = xgw_channel_get(&line_channels->channels, XGW_CHANNEL_NEXT);
        if (previous != NULL &&
            strcmp(previous->peer_host, peer_host) == 0 &&
            previous->peer_port == peer_port) {
            for (slot_idx = 0U; slot_idx < (size_t) XGW_SESSION_SLOT_COUNT; ++slot_idx) {
                if (previous->pending_slots[slot_idx] == session) {
                    xgw_channel_promote_slot(previous, (xgw_session_slot_t) slot_idx);
                }
            }
            previous->pending_session = previous->pending_slots[XGW_SESSION_SLOT_BULK];
            previous->active_session = previous->active_slots[XGW_SESSION_SLOT_BULK];
            runtime_channel_promote_established(line_channels->line_id, previous);
        }
        if (next != NULL &&
            strcmp(next->peer_host, peer_host) == 0 &&
            next->peer_port == peer_port) {
            for (slot_idx = 0U; slot_idx < (size_t) XGW_SESSION_SLOT_COUNT; ++slot_idx) {
                if (next->pending_slots[slot_idx] == session) {
                    xgw_channel_promote_slot(next, (xgw_session_slot_t) slot_idx);
                }
            }
            next->pending_session = next->pending_slots[XGW_SESSION_SLOT_BULK];
            next->active_session = next->active_slots[XGW_SESSION_SLOT_BULK];
            runtime_channel_promote_established(line_channels->line_id, next);
        }
    }
}

static int runtime_channel_tick_one(xgw_udp_socket_t *udp,
                                    xgw_dataplane_t *dp,
                                    const xgw_runtime_config_t *config,
                                    const xgw_negotiation_info_t *negotiation,
                                    const xgw_obfs_t *obfs,
                                    int hop_obfs,
                                    const xgw_runtime_line_channels_t *line_channels,
                                    xgw_channel_direction_t direction,
                                    char *error,
                                    size_t error_len) {
    xgw_neighbor_channel_t *channel;
    if (line_channels == NULL || !line_channels->valid) {
        return 1;
    }
    channel = xgw_channel_get((xgw_channel_set_t *) &line_channels->channels, direction);
    if (channel == NULL ||
        channel->peer_host[0] == '\0' || channel->peer_port == 0U) {
        return 1;
    }
    {
        size_t slot_idx;
        for (slot_idx = 0U; slot_idx < (size_t) XGW_SESSION_SLOT_COUNT; ++slot_idx) {
            xgw_session_slot_t slot = (xgw_session_slot_t) slot_idx;
            xgw_session_t *existing = get_send_session(dp,
                                                       config,
                                                       line_channels->line_id,
                                                       direction,
                                                       slot,
                                                       channel->peer_host,
                                                       channel->peer_port);
            if (existing != NULL) {
                xgw_channel_set_pending_slot(channel, slot, existing);
            }
        }
    }
    {
        size_t slot_idx;
        int any_handshaking = 0;
        for (slot_idx = 0U; slot_idx < (size_t) XGW_SESSION_SLOT_COUNT; ++slot_idx) {
            xgw_session_slot_t slot = (xgw_session_slot_t) slot_idx;
            xgw_session_t *slot_session = channel->pending_slots[slot];
            if (slot_session == NULL) {
                set_error(error, error_len, "channel slot session alloc failed");
                return 0;
            }
            if (slot_session->remote_addr.host[0] == '\0' ||
                strcmp(slot_session->remote_addr.host, channel->peer_host) != 0 ||
                slot_session->remote_addr.port != channel->peer_port) {
                slot_session = get_send_session(dp,
                                                config,
                                                line_channels->line_id,
                                                direction,
                                                slot,
                                                channel->peer_host,
                                                channel->peer_port);
                if (slot_session == NULL) {
                    set_error(error, error_len, "channel slot session rebind failed");
                    return 0;
                }
                xgw_channel_set_pending_slot(channel, slot, slot_session);
            }
            if (slot == XGW_SESSION_SLOT_CONTROL) {
                maybe_refresh_control_session(slot_session,
                                              config,
                                              channel->peer_host,
                                              channel->peer_port);
            }
            if (!maybe_send_handshake(udp,
                                      slot_session,
                                      config,
                                      negotiation,
                                      obfs,
                                      hop_obfs,
                                      channel->peer_host,
                                      channel->peer_port)) {
                set_error(error, error_len, "channel slot hello failed");
                return 0;
            }
            if (!maybe_send_keepalive(udp,
                                      slot_session,
                                      config,
                                      obfs,
                                      hop_obfs,
                                      channel->peer_host,
                                      channel->peer_port)) {
                set_error(error, error_len, "channel slot keepalive failed");
                return 0;
            }
            if (channel->active_slots[slot] == NULL) {
                any_handshaking = 1;
            }
        }
        channel->pending_session = channel->pending_slots[XGW_SESSION_SLOT_BULK];
        channel->active_session = channel->active_slots[XGW_SESSION_SLOT_BULK];
        channel->state = any_handshaking ? XGW_CHANNEL_STATE_HANDSHAKING : XGW_CHANNEL_STATE_ESTABLISHED;
    }
    runtime_channel_promote_established(line_channels->line_id, channel);
    return 1;
}

static int runtime_channel_manager_tick(xgw_udp_socket_t *udp,
                                        xgw_dataplane_t *dp,
                                        const xgw_runtime_config_t *config,
                                        const xgw_negotiation_info_t *negotiation,
                                        const xgw_obfs_t *obfs,
                                        int hop_obfs,
                                        const xgw_runtime_channels_t *channels,
                                        char *error,
                                        size_t error_len) {
    if (udp == NULL || config == NULL || channels == NULL) {
        return 1;
    }
    if (!runtime_channel_tick_one(udp, dp, config, negotiation, obfs, hop_obfs, &channels->active, XGW_CHANNEL_NEXT, error, error_len)) {
        return 0;
    }
    if (!runtime_channel_tick_one(udp, dp, config, negotiation, obfs, hop_obfs, &channels->active, XGW_CHANNEL_PREVIOUS, error, error_len)) {
        return 0;
    }
    if (!runtime_channel_tick_one(udp, dp, config, negotiation, obfs, hop_obfs, &channels->candidate, XGW_CHANNEL_NEXT, error, error_len)) {
        return 0;
    }
    if (!runtime_channel_tick_one(udp, dp, config, negotiation, obfs, hop_obfs, &channels->candidate, XGW_CHANNEL_PREVIOUS, error, error_len)) {
        return 0;
    }
    if (!runtime_channel_tick_one(udp, dp, config, negotiation, obfs, hop_obfs, &channels->draining, XGW_CHANNEL_NEXT, error, error_len)) {
        return 0;
    }
    if (!runtime_channel_tick_one(udp, dp, config, negotiation, obfs, hop_obfs, &channels->draining, XGW_CHANNEL_PREVIOUS, error, error_len)) {
        return 0;
    }
    return 1;
}

static const xgw_endpoint_t *select_forward_target(const xgw_node_hops_t *hops,
                                                   const xgw_runtime_config_t *config,
                                                   const xgw_packet_t *packet,
                                                   const xgw_session_t *packet_session) {
    (void) config;
    if (hops == NULL || packet == NULL) {
        return NULL;
    }
    if (hops->previous != NULL && endpoint_matches_packet(hops->previous, packet)) {
        if (XGW_RT_VERBOSE()) printf("runtime.forward.select from=previous to=next remote=%s:%u\n",
               packet->remote_host,
               packet->remote_port);
        fflush(stdout);
        return hops->next;
    }
    if (hops->next != NULL && endpoint_matches_packet(hops->next, packet)) {
        if (XGW_RT_VERBOSE()) printf("runtime.forward.select from=next to=previous remote=%s:%u\n",
               packet->remote_host,
               packet->remote_port);
        fflush(stdout);
        return hops->previous;
    }
    if (packet_session != NULL &&
        packet_session->remote_addr.host[0] != '\0' &&
        packet_session->remote_addr.port != 0U) {
        if (endpoint_matches_host_port(hops->previous,
                                       packet_session->remote_addr.host,
                                       packet_session->remote_addr.port)) {
            if (XGW_RT_VERBOSE()) printf("runtime.forward.select via=session from=previous to=next packet_remote=%s:%u session_peer=%s:%u\n",
                   packet->remote_host,
                   packet->remote_port,
                   packet_session->remote_addr.host,
                   packet_session->remote_addr.port);
            fflush(stdout);
            return hops->next;
        }
        if (endpoint_matches_host_port(hops->next,
                                       packet_session->remote_addr.host,
                                       packet_session->remote_addr.port)) {
            if (XGW_RT_VERBOSE()) printf("runtime.forward.select via=session from=next to=previous packet_remote=%s:%u session_peer=%s:%u\n",
                   packet->remote_host,
                   packet->remote_port,
                   packet_session->remote_addr.host,
                   packet_session->remote_addr.port);
            fflush(stdout);
            return hops->previous;
        }
    }
    if (XGW_RT_VERBOSE()) printf("runtime.forward.select fallback to=%s remote=%s:%u\n",
           hops->next != NULL ? "next" : "previous",
           packet->remote_host,
           packet->remote_port);
    fflush(stdout);
    return hops->next != NULL ? hops->next : hops->previous;
}

static xgw_session_t *get_send_session(xgw_dataplane_t *dp,
                                       const xgw_runtime_config_t *config,
                                       const char *line_id,
                                       xgw_channel_direction_t direction,
                                       xgw_session_slot_t slot,
                                       const char *host,
                                       uint16_t port) {
    char scope[160];
    char segment[48];
    xgw_transport_segment_t transport_segment;
    uint32_t session_id;
    xgw_session_t *session = xgw_session_upsert(&dp->sessions,
                                                1000U,
                                                config->allow_policy.group_id,
                                                config->tun_addr,
                                                host,
                                                port,
                                                time(NULL));
    (void) session;
    transport_segment = runtime_transport_segment(config, direction);
    runtime_session_segment_key(config, direction, segment, sizeof(segment));
    snprintf(scope,
             sizeof(scope),
             "%s|seg=%s|slot=%d",
             line_id == NULL ? "" : line_id,
             segment,
             (int) slot);
    session_id = 1000U + (stable_hash32_runtime(scope) % 60000U);
    session = xgw_session_upsert(&dp->sessions,
                                                session_id,
                                                config->allow_policy.group_id,
                                                config->tun_addr,
                                                host,
                                                port,
                                                time(NULL));
    xgw_session_set_transport_scope(session, line_id, transport_segment, slot, host, port);
    if (session != NULL && session->security.static_ready == 0 && config->auth_token[0] != '\0') {
        xgw_security_init(&session->security, config->auth_token);
    }
    if (session != NULL) {
        if (XGW_RT_VERBOSE()) printf("runtime.send_session.alloc line=%s dir=%d seg=%s slot=%s transport_session_id=%u peer=%s:%u\n",
               line_id == NULL ? "" : line_id,
               (int) direction,
               segment,
               runtime_session_slot_name(slot),
               session->id,
               host == NULL ? "" : host,
               port);
        fflush(stdout);
    }
    return session;
}

static void runtime_session_segment_key(const xgw_runtime_config_t *config,
                                        xgw_channel_direction_t direction,
                                        char *out,
                                        size_t out_len) {
    const char *segment = "generic";
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (config != NULL) {
        if ((config->role == XGW_ROLE_INGRESS && direction == XGW_CHANNEL_NEXT) ||
            (config->role == XGW_ROLE_RELAY && direction == XGW_CHANNEL_PREVIOUS)) {
            segment = "ingress-relay";
        } else if ((config->role == XGW_ROLE_RELAY && direction == XGW_CHANNEL_NEXT) ||
                   (config->role == XGW_ROLE_EGRESS && direction == XGW_CHANNEL_PREVIOUS)) {
            segment = "relay-egress";
        } else if ((config->role == XGW_ROLE_MOBILE && direction == XGW_CHANNEL_NEXT) ||
                   (config->role == XGW_ROLE_INGRESS && direction == XGW_CHANNEL_PREVIOUS)) {
            segment = "mobile-ingress";
        }
    }
    snprintf(out, out_len, "%s", segment);
}

static xgw_transport_segment_t runtime_transport_segment(const xgw_runtime_config_t *config,
                                                         xgw_channel_direction_t direction) {
    if (config == NULL) {
        return XGW_TRANSPORT_SEGMENT_UNKNOWN;
    }
    if ((config->role == XGW_ROLE_INGRESS && direction == XGW_CHANNEL_NEXT) ||
        (config->role == XGW_ROLE_RELAY && direction == XGW_CHANNEL_PREVIOUS)) {
        return XGW_TRANSPORT_SEGMENT_INGRESS_RELAY;
    }
    if ((config->role == XGW_ROLE_RELAY && direction == XGW_CHANNEL_NEXT) ||
        (config->role == XGW_ROLE_EGRESS && direction == XGW_CHANNEL_PREVIOUS)) {
        return XGW_TRANSPORT_SEGMENT_RELAY_EGRESS;
    }
    if ((config->role == XGW_ROLE_MOBILE && direction == XGW_CHANNEL_NEXT) ||
        (config->role == XGW_ROLE_INGRESS && direction == XGW_CHANNEL_PREVIOUS)) {
        return XGW_TRANSPORT_SEGMENT_MOBILE_INGRESS;
    }
    return XGW_TRANSPORT_SEGMENT_UNKNOWN;
}

static xgw_bridge_target_entry_t *find_bridge_target_entry(xgw_bridge_target_entry_t *entries, uint32_t stream_id) {
    size_t i;
    if (entries == NULL) {
        return NULL;
    }
    for (i = 0; i < XGW_BRIDGE_TARGET_MAP_SIZE; ++i) {
        if (entries[i].active && entries[i].stream_id == stream_id) {
            return &entries[i];
        }
    }
    return NULL;
}

static xgw_bridge_target_entry_t *find_bridge_target_from_payload(xgw_bridge_target_entry_t *entries,
                                                                  const uint8_t *payload,
                                                                  size_t payload_len) {
    uint32_t stream_id = 0U;
    if (entries == NULL || payload == NULL || payload_len == 0U) {
        return NULL;
    }
    if (!parse_stream_id_from_bridge_payload(payload, payload_len, &stream_id)) {
        return NULL;
    }
    return find_bridge_target_entry(entries, stream_id);
}

static void stream_sched_forget(xgw_stream_scheduler_t *sched, uint32_t stream_id) {
    xgw_stream_sched_entry_t *entry = stream_sched_find(sched, stream_id);
    if (entry != NULL) {
        sched_deactivate(sched, entry);
    }
}

static void forget_bridge_target(xgw_bridge_target_entry_t *entries, uint32_t stream_id) {
    xgw_bridge_target_entry_t *entry = find_bridge_target_entry(entries, stream_id);
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    }
}

static void remember_bridge_target(xgw_bridge_target_entry_t *entries,
                                   uint32_t stream_id,
                                   const char *host,
                                   uint16_t port,
                                   const uint8_t *meta,
                                   size_t meta_len) {
    size_t i;
    xgw_bridge_target_entry_t *entry = find_bridge_target_entry(entries, stream_id);
    if (entry == NULL && entries != NULL) {
        for (i = 0; i < XGW_BRIDGE_TARGET_MAP_SIZE; ++i) {
            if (!entries[i].active) {
                entry = &entries[i];
                memset(entry, 0, sizeof(*entry));
                entry->active = 1;
                entry->stream_id = stream_id;
                break;
            }
        }
    }
    if (entry == NULL) {
        return;
    }
    snprintf(entry->target_host, sizeof(entry->target_host), "%s", host == NULL ? "" : host);
    entry->target_port = port;
    if (meta != NULL && meta_len >= 8U) {
        entry->flow_class = meta[0];
        entry->priority = meta[1];
        entry->budget_flags = meta[2];
        entry->preferred_copies = meta[3];
        entry->read_timeout_ms = read_bridge_timeout_runtime(meta, meta_len, 4U, 4U);
        entry->idle_after_first_byte_ms = read_bridge_timeout_runtime(meta, meta_len, 6U, 8U);
        if (meta_len > 12U) {
            snprintf(entry->front_session_id, sizeof(entry->front_session_id), "%s", (const char *) (meta + 12U));
        }
        if (meta_len > 52U) {
            snprintf(entry->frontend, sizeof(entry->frontend), "%s", (const char *) (meta + 52U));
        }
        if (meta_len > 68U) {
            snprintf(entry->route_name, sizeof(entry->route_name), "%s", (const char *) (meta + 68U));
        }
        if (meta_len > 92U) {
            snprintf(entry->line_id, sizeof(entry->line_id), "%s", (const char *) (meta + 92U));
        }
    }
    if (entry->opened_us == 0U) {
        entry->opened_us = monotonic_us();
    }
    entry->close_reason[0] = '\0';
}

static void update_bridge_target_map(xgw_bridge_target_entry_t *entries, const uint8_t *payload, size_t payload_len) {
    uint32_t magic;
    uint32_t stream_id;
    uint8_t kind;
    if (entries == NULL || payload == NULL || payload_len < 9U) {
        return;
    }
    magic = read_be32_runtime(payload);
    if (magic != 0x58474231U) {
        return;
    }
    stream_id = read_be32_runtime(payload + 4U);
    kind = payload[8];
    if ((kind == 1U || kind == 4U) && payload_len >= 13U) {
        uint16_t host_len = read_be16_runtime(payload + 9U);
        if (payload_len >= 13U + host_len && host_len < 256U) {
            char host[256];
            uint16_t port;
            const uint8_t *meta = NULL;
            size_t meta_len = 0U;
            memcpy(host, payload + 11U, host_len);
            host[host_len] = '\0';
            port = read_be16_runtime(payload + 11U + host_len);
            if (payload_len >= 13U + host_len + 8U) {
                meta = payload + 13U + host_len;
                meta_len = payload_len - (13U + host_len);
            }
            remember_bridge_target(entries, stream_id, host, port, meta, meta_len);
        }
        return;
    }
    (void) kind;
}

static void log_bridge_target_map(const char *prefix,
                                  xgw_bridge_target_entry_t *entries,
                                  const uint8_t *payload,
                                  size_t payload_len) {
    uint32_t magic;
    uint32_t stream_id;
    uint8_t kind;
    xgw_bridge_target_entry_t *entry;
    if (!XGW_RT_VERBOSE()) {
        return;
    }
    if (entries == NULL || payload == NULL || payload_len < 9U) {
        return;
    }
    magic = read_be32_runtime(payload);
    if (magic != 0x58474231U) {
        return;
    }
    stream_id = read_be32_runtime(payload + 4U);
    kind = payload[8];
    entry = find_bridge_target_entry(entries, stream_id);
    if (entry != NULL) {
        printf("%s stream=%u kind=%u target=%s:%u flow_class=%u priority=%u frontend=%s front_session_id=%s route=%s line=%s\n",
               prefix == NULL ? "bridge.map" : prefix,
               stream_id,
               (unsigned int) kind,
               entry->target_host,
               entry->target_port,
               (unsigned int) entry->flow_class,
               (unsigned int) entry->priority,
               entry->frontend,
               entry->front_session_id,
               entry->route_name,
               entry->line_id);
    } else {
        printf("%s stream=%u kind=%u target=?\n",
               prefix == NULL ? "bridge.map" : prefix,
               stream_id,
               (unsigned int) kind);
    }
    fflush(stdout);
}

static int bridge_payload_from_previous(const xgw_node_hops_t *hops,
                                        const xgw_packet_t *packet,
                                        const xgw_session_t *packet_session) {
    if (hops == NULL || packet == NULL) {
        return 0;
    }
    if (endpoint_matches_packet(hops->previous, packet)) {
        return 1;
    }
    if (packet_session != NULL &&
        packet_session->remote_addr.host[0] != '\0' &&
        packet_session->remote_addr.port != 0U &&
        endpoint_matches_host_port(hops->previous,
                                   packet_session->remote_addr.host,
                                   packet_session->remote_addr.port)) {
        return 1;
    }
    return 0;
}

static int bridge_payload_from_next(const xgw_node_hops_t *hops,
                                    const xgw_packet_t *packet,
                                    const xgw_session_t *packet_session) {
    if (hops == NULL || packet == NULL) {
        return 0;
    }
    if (endpoint_matches_packet(hops->next, packet)) {
        return 1;
    }
    if (packet_session != NULL &&
        packet_session->remote_addr.host[0] != '\0' &&
        packet_session->remote_addr.port != 0U &&
        endpoint_matches_host_port(hops->next,
                                   packet_session->remote_addr.host,
                                   packet_session->remote_addr.port)) {
        return 1;
    }
    return 0;
}

static void note_relay_target_stage(xgw_bridge_target_entry_t *entries,
                                    const xgw_runtime_config_t *config,
                                    const xgw_node_hops_t *hops,
                                    const xgw_packet_t *packet,
                                    const xgw_session_t *packet_session,
                                    const uint8_t *payload,
                                    size_t payload_len) {
    uint32_t stream_id = 0U;
    uint8_t kind = 0U;
    uint16_t data_len = 0U;
    xgw_bridge_target_entry_t *entry;
    int from_previous;
    int from_next;
    if (entries == NULL || config == NULL || config->role != XGW_ROLE_RELAY) {
        return;
    }
    if (!parse_bridge_payload_meta(payload, payload_len, &stream_id, &kind, &data_len)) {
        return;
    }
    entry = find_bridge_target_entry(entries, stream_id);
    if (entry == NULL) {
        return;
    }
    from_previous = bridge_payload_from_previous(hops, packet, packet_session);
    from_next = bridge_payload_from_next(hops, packet, packet_session);
    if ((kind == 2U || kind == 5U) && from_previous && entry->relay_first_packet_us == 0U) {
        entry->relay_first_packet_us = monotonic_us();
        printf("relay.phase.first_packet stream=%u target=%s:%u elapsed_ms=%llu bytes=%u direction=ingress_to_egress\n",
               entry->stream_id,
               entry->target_host,
               entry->target_port,
               elapsed_ms_runtime(entry->opened_us),
               (unsigned int) data_len);
        fflush(stdout);
        if (contains_ignore_case_runtime(entry->target_host, "mon-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "api-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "tnc-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "frontier.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "log-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "gecko-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "jsb-boot.tiktokv.com")) {
            printf("tiktok.trace.relay.first_packet stream=%u target=%s:%u elapsed_ms=%llu bytes=%u direction=ingress_to_egress\n",
                   entry->stream_id,
                   entry->target_host,
                   entry->target_port,
                   elapsed_ms_runtime(entry->opened_us),
                   (unsigned int) data_len);
            fflush(stdout);
        }
        return;
    }
    if ((kind == 2U || kind == 5U) && from_next && entry->relay_first_byte_us == 0U) {
        entry->relay_first_byte_us = monotonic_us();
        printf("relay.phase.first_byte stream=%u target=%s:%u elapsed_ms=%llu bytes=%u direction=egress_to_ingress\n",
               entry->stream_id,
               entry->target_host,
               entry->target_port,
               elapsed_ms_runtime(entry->opened_us),
               (unsigned int) data_len);
        fflush(stdout);
        if (contains_ignore_case_runtime(entry->target_host, "mon-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "api-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "tnc-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "frontier.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "log-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "gecko-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "jsb-boot.tiktokv.com")) {
            printf("tiktok.trace.relay.first_byte stream=%u target=%s:%u elapsed_ms=%llu bytes=%u direction=egress_to_ingress\n",
                   entry->stream_id,
                   entry->target_host,
                   entry->target_port,
                   elapsed_ms_runtime(entry->opened_us),
                   (unsigned int) data_len);
            fflush(stdout);
        }
        return;
    }
    if ((kind == 3U || kind == 6U) && (from_previous || from_next)) {
        unsigned long long first_packet_ms = 0ULL;
        unsigned long long first_byte_ms = 0ULL;
        snprintf(entry->close_reason,
                 sizeof(entry->close_reason),
                 "%s",
                 from_previous ? "close_from_ingress" : "close_from_egress");
        if (entry->relay_first_packet_us > entry->opened_us && entry->opened_us > 0U) {
            first_packet_ms = (unsigned long long) ((entry->relay_first_packet_us - entry->opened_us) / 1000ULL);
        }
        if (entry->relay_first_byte_us > entry->opened_us && entry->opened_us > 0U) {
            first_byte_ms = (unsigned long long) ((entry->relay_first_byte_us - entry->opened_us) / 1000ULL);
        }
        printf("relay.phase.close stream=%u target=%s:%u lifetime_ms=%llu first_packet_ms=%llu first_byte_ms=%llu reason=%s\n",
               entry->stream_id,
               entry->target_host,
               entry->target_port,
               elapsed_ms_runtime(entry->opened_us),
               first_packet_ms,
               first_byte_ms,
               entry->close_reason);
        fflush(stdout);
        if (contains_ignore_case_runtime(entry->target_host, "mon-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "api-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "tnc-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "frontier.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "log-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "gecko-boot.tiktokv.com") ||
            contains_ignore_case_runtime(entry->target_host, "jsb-boot.tiktokv.com")) {
            printf("tiktok.trace.relay.close stream=%u target=%s:%u lifetime_ms=%llu first_packet_ms=%llu first_byte_ms=%llu reason=%s\n",
                   entry->stream_id,
                   entry->target_host,
                   entry->target_port,
                   elapsed_ms_runtime(entry->opened_us),
                   first_packet_ms,
                   first_byte_ms,
                   entry->close_reason);
            fflush(stdout);
        }
        forget_bridge_target(entries, stream_id);
    }
}

static void forget_bridge_target_if_close(xgw_bridge_target_entry_t *entries,
                                          const uint8_t *payload,
                                          size_t payload_len) {
    uint32_t stream_id = 0U;
    uint8_t kind = 0U;
    if (!parse_bridge_payload_meta(payload, payload_len, &stream_id, &kind, NULL)) {
        return;
    }
    if (kind == 3U || kind == 6U) {
        forget_bridge_target(entries, stream_id);
    }
}

static void forget_scheduler_if_close(xgw_stream_scheduler_t *sched,
                                      const uint8_t *payload,
                                      size_t payload_len) {
    uint32_t stream_id = 0U;
    uint8_t kind = 0U;
    if (sched == NULL) {
        return;
    }
    if (!parse_bridge_payload_meta(payload, payload_len, &stream_id, &kind, NULL)) {
        return;
    }
    if (kind == 3U || kind == 6U) {
        stream_sched_forget(sched, stream_id);
    }
}

/* per-stream xport 事件驱动主回收:流 CLOSE(6)/HALF_CLOSE(3) 时立即释放该 stream 的 xport,
 * 与 forget_bridge_target/forget_scheduler 在同一批 close 点同步调用。超时 reap 仅作兜底。 */
static void forget_xport_if_close(xgw_stream_xport_pool_t *pool,
                                  const uint8_t *payload,
                                  size_t payload_len) {
    uint32_t stream_id = 0U;
    uint8_t kind = 0U;
    if (pool == NULL) {
        return;
    }
    if (!parse_bridge_payload_meta(payload, payload_len, &stream_id, &kind, NULL)) {
        return;
    }
    /* CLOSE 事件主回收(对齐 hy2:流结束即销毁状态,新流必拿全新状态)。
     * kind 取值见 xgw_local_adapter.h: TCP_CLOSE=3, UDP_CLOSE=6, HALF_CLOSE=7。
     * 之前误用 kind==6 只覆盖 UDP_CLOSE,导致 TCP 流(TikTok 视频)的 xport 从不在 CLOSE 回收、
     * 只能靠 reap 兜底,叠加 reap 过短 → 序号错乱卡死。此处覆盖 TCP+UDP CLOSE。
     * HALF_CLOSE(7) 是单向半关,反方向可能仍有数据,不在此回收,交 reap 兜底。 */
    if (kind == XGW_LOCAL_ADAPTER_KIND_TCP_CLOSE || kind == XGW_LOCAL_ADAPTER_KIND_UDP_CLOSE) {
        xgw_stream_xport_forget_stream(pool, stream_id);
    }
}

static void log_bridge_payload_summary(const char *prefix, const uint8_t *payload, size_t payload_len) {
    uint32_t magic;
    uint32_t stream_id;
    uint8_t kind;
    if (!XGW_RT_VERBOSE()) {
        return;
    }
    if (payload == NULL || payload_len < 9U) {
        return;
    }
    magic = ((uint32_t) payload[0] << 24U) |
            ((uint32_t) payload[1] << 16U) |
            ((uint32_t) payload[2] << 8U) |
            (uint32_t) payload[3];
    if (magic != 0x58474231U) {
        return;
    }
    stream_id = ((uint32_t) payload[4] << 24U) |
                ((uint32_t) payload[5] << 16U) |
                ((uint32_t) payload[6] << 8U) |
                (uint32_t) payload[7];
    kind = payload[8];
    if ((kind == 1U || kind == 4U) && payload_len >= 13U) {
        uint16_t host_len = (uint16_t) (((uint16_t) payload[9] << 8U) | (uint16_t) payload[10]);
        if (payload_len >= 13U + host_len && host_len < 256U) {
            char host[256];
            uint16_t target_port;
            memcpy(host, payload + 11U, host_len);
            host[host_len] = '\0';
            target_port = (uint16_t) (((uint16_t) payload[11U + host_len] << 8U) |
                                      (uint16_t) payload[12U + host_len]);
            printf("%s stream=%u kind=%s target=%s:%u payload_len=%zu\n",
                   prefix == NULL ? "bridge.summary" : prefix,
                   stream_id,
                   kind == 4U ? "udp_open" : "open",
                   host,
                   target_port,
                   payload_len);
            fflush(stdout);
        }
        return;
    }
    if (kind == 2U && payload_len >= 11U) {
        uint16_t data_len = (uint16_t) (((uint16_t) payload[9] << 8U) | (uint16_t) payload[10]);
        printf("%s stream=%u kind=data data_len=%u payload_len=%zu\n",
               prefix == NULL ? "bridge.summary" : prefix,
               stream_id,
               (unsigned int) data_len,
               payload_len);
        fflush(stdout);
        return;
    }
    if (kind == 5U && payload_len >= 13U) {
        uint16_t addr_len = (uint16_t) (((uint16_t) payload[9] << 8U) | (uint16_t) payload[10]);
        if (payload_len >= 13U + addr_len) {
            uint16_t data_len = (uint16_t) (((uint16_t) payload[11U + addr_len] << 8U) |
                                            (uint16_t) payload[12U + addr_len]);
            printf("%s stream=%u kind=udp_data addr_len=%u data_len=%u payload_len=%zu\n",
                   prefix == NULL ? "bridge.summary" : prefix,
                   stream_id,
                   (unsigned int) addr_len,
                   (unsigned int) data_len,
                   payload_len);
            fflush(stdout);
        }
        return;
    }
    if (kind == 3U || kind == 6U) {
        printf("%s stream=%u kind=close payload_len=%zu\n",
               prefix == NULL ? "bridge.summary" : prefix,
               stream_id,
               payload_len);
        fflush(stdout);
    }
}

static void log_session_summary(const char *prefix, const xgw_session_t *session) {
    char sec_summary[192];
    char scope_summary[160];
    if (!XGW_RT_VERBOSE()) {
        return;
    }
    if (session == NULL) {
        printf("%s session=null\n", prefix == NULL ? "session" : prefix);
        fflush(stdout);
        return;
    }
    xgw_security_debug_summary(&session->security, sec_summary, sizeof(sec_summary));
    printf("%s transport_session_id=%u remote=%s:%u state=%d %s %s\n",
           prefix == NULL ? "session" : prefix,
           session->id,
           session->remote_addr.host,
           session->remote_addr.port,
           session->control_state,
           xgw_transport_scope_summary(&session->scope, scope_summary, sizeof(scope_summary)),
           sec_summary);
    fflush(stdout);
}

static void log_cc_snapshot(const xgw_session_t *session) {
    xgw_cc_stats_t stats;
    if (session == NULL || !session->active) {
        return;
    }
    xgw_cc_snapshot(&session->cc, &stats);
    printf("runtime.cc session=%u peer=%s:%u mode=%s bbr_mode=%s round=%u inflight=%llu cwnd=%llu pacing_bps=%llu bw_bps=%llu srtt_us=%llu min_rtt_us=%llu acked=%llu lost=%llu outstanding=%zu fec_parity=%u\n",
           session->id,
           session->remote_addr.host,
           session->remote_addr.port,
           xgw_congestion_mode_name(session->cc.mode),
           xgw_bbr_mode_name(session->cc.bbr_mode),
           session->cc.round_count,
           (unsigned long long) stats.inflight_bytes,
           (unsigned long long) stats.cwnd_bytes,
           (unsigned long long) stats.pacing_rate_bps,
           (unsigned long long) stats.max_bandwidth_bps,
           (unsigned long long) session->cc.srtt_us,
           (unsigned long long) stats.min_rtt_us,
           (unsigned long long) stats.acked_packets,
           (unsigned long long) stats.lost_packets,
           session->outstanding_count,
           session->last_fec_parity_used);
    fflush(stdout);
}

static int write_runtime_summary_json(const xgw_runtime_config_t *config,
                                      const xgw_route_manager_t *routes,
                                      const xgw_line_runtime_t *active_line,
                                      const xgw_dataplane_t *dp,
                                      const xgw_stream_scheduler_t *sched,
                                      const xgw_traffic_limiter_t *traffic_limiter,
                                      time_t now) {
    char tmp_path[224];
    FILE *fp;
    size_t i;
    size_t written = 0U;
    if (config == NULL || dp == NULL || config->tuning.summary_path[0] == '\0') {
        return 1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", config->tuning.summary_path);
    fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        return 0;
    }
    fprintf(fp, "{\n");
    fprintf(fp, "  \"timestamp\": %lld,\n", (long long) now);
    fprintf(fp, "  \"node_name\": \"%s\",\n", config->node_name);
    fprintf(fp, "  \"role\": \"%s\",\n", xgw_role_name(config->role));
    fprintf(fp, "  \"transport\": \"%s\",\n", config->transport);
    fprintf(fp, "  \"proxy_mode\": \"%s\",\n", config->proxy_mode);
    fprintf(fp, "  \"active_line\": \"%s\",\n", runtime_line_id(active_line));
    fprintf(fp, "  \"candidate_line\": \"%s\",\n",
            xgw_route_candidate(routes) == NULL ? "" : runtime_line_id(xgw_route_candidate(routes)));
    fprintf(fp, "  \"draining_line\": \"%s\",\n",
            xgw_route_draining(routes) == NULL ? "" : runtime_line_id(xgw_route_draining(routes)));
    fprintf(fp, "  \"traffic_limit\": {\n");
    fprintf(fp, "    \"enabled\": %s,\n", traffic_limiter != NULL && traffic_limiter->enabled ? "true" : "false");
    fprintf(fp, "    \"baseline_bps\": %llu,\n", (unsigned long long) (traffic_limiter == NULL ? 0U : traffic_limiter->baseline_bps));
    fprintf(fp, "    \"burst_bps\": %llu,\n", (unsigned long long) (traffic_limiter == NULL ? 0U : traffic_limiter->burst_bps));
    fprintf(fp, "    \"burst_budget_bytes\": %llu\n", (unsigned long long) (traffic_limiter == NULL ? 0U : traffic_limiter->capacity_bytes));
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"fec\": {\n");
    fprintf(fp, "    \"enabled\": %s,\n", dp->fec.enabled ? "true" : "false");
    fprintf(fp, "    \"groups\": %zu,\n", dp->fec.group_count);
    fprintf(fp, "    \"completed\": %zu,\n", dp->fec.completed_count);
    fprintf(fp, "    \"data_shards\": %u,\n", dp->fec.data_shards);
    fprintf(fp, "    \"parity_shards\": %u\n", dp->fec.parity_shards);
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"sessions\": [\n");
    for (i = 0; i < dp->sessions.count; ++i) {
        const xgw_session_t *session = &dp->sessions.sessions[i];
        xgw_cc_stats_t stats;
        if (!session->active) {
            continue;
        }
        xgw_cc_snapshot(&session->cc, &stats);
        if (written > 0U) {
            fprintf(fp, ",\n");
        }
        fprintf(fp,
                "    {\"id\": %u, \"peer\": \"%s:%u\", \"state\": %d, \"mode\": \"%s\", \"bbr_mode\": \"%s\", \"round\": %u, \"recovery_rounds_left\": %u, \"full_bw_rounds\": %u, \"inflight\": %llu, \"cwnd\": %llu, \"pacing_bps\": %llu, \"bandwidth_bps\": %llu, \"srtt_us\": %llu, \"min_rtt_us\": %llu, \"acked\": %llu, \"lost\": %llu, \"outstanding\": %zu, \"fec_parity\": %u}",
                session->id,
                session->remote_addr.host,
                session->remote_addr.port,
                session->control_state,
                xgw_congestion_mode_name(session->cc.mode),
                xgw_bbr_mode_name(session->cc.bbr_mode),
                session->cc.round_count,
                session->cc.recovery_rounds_left,
                session->cc.full_bw_rounds,
                (unsigned long long) stats.inflight_bytes,
                (unsigned long long) stats.cwnd_bytes,
                (unsigned long long) stats.pacing_rate_bps,
                (unsigned long long) stats.max_bandwidth_bps,
                (unsigned long long) session->cc.srtt_us,
                (unsigned long long) stats.min_rtt_us,
                (unsigned long long) stats.acked_packets,
                (unsigned long long) stats.lost_packets,
                session->outstanding_count,
                session->last_fec_parity_used);
        written++;
    }
    fprintf(fp, "\n  ],\n");
    fprintf(fp, "  \"streams\": [\n");
    written = 0U;
    if (sched != NULL) {
        for (i = 0U; i < sched->active_n; ++i) {
            const xgw_stream_sched_entry_t *entry = &sched->entries[sched->active_idx[i]];
            if (!entry->active) {
                continue;
            }
            if (written > 0U) {
                fprintf(fp, ",\n");
            }
            fprintf(fp,
                    "    {\"stream_id\": %u, \"target\": \"%s:%u\", \"frontend\": \"%s\", \"front_session_id\": \"%s\", \"route_name\": \"%s\", \"line_id\": \"%s\", \"channel_direction\": %d, \"session_slot\": \"%s\", \"flow_class\": %u, \"priority\": %u, \"preferred_copies\": %u, \"pending\": %u, \"payload_len\": %zu, \"inflight_bytes\": %llu, \"send_credit_bytes\": %llu, \"ack_credit_frames\": %u, \"deficit_bytes\": %llu, \"starvation_rounds\": %u, \"loss_ppm\": %u, \"recovered_ppm\": %u, \"parity_budget\": %u, \"feedback_cadence_ms\": %u}",
                    entry->stream_id,
                    entry->target_host,
                    entry->target_port,
                    entry->frontend,
                    entry->front_session_id,
                    entry->route_name,
                    entry->line_id,
                    (int) entry->send_direction,
                    runtime_session_slot_name(entry->session_slot),
                    entry->hint.flow_class,
                    entry->hint.priority,
                    entry->hint.preferred_copies,
                    entry->pending,
                    entry->payload_len,
                    (unsigned long long) entry->inflight_bytes,
                    (unsigned long long) entry->send_credit_bytes,
                    entry->ack_credit_frames,
                    (unsigned long long) entry->deficit_bytes,
                    entry->starvation_rounds,
                    entry->rolling_loss_ppm,
                    entry->rolling_recovered_ppm,
                    entry->parity_budget,
                    entry->feedback_cadence_ms);
            written++;
        }
    }
    fprintf(fp, "\n  ]\n");
    fprintf(fp, "}\n");
    fclose(fp);
    remove(config->tuning.summary_path);
    return rename(tmp_path, config->tuning.summary_path) == 0;
}

static xgw_session_t *find_established_peer_session(xgw_dataplane_t *dp) {
    size_t i;
    if (dp == NULL) {
        return NULL;
    }
    for (i = 0; i < dp->sessions.count; ++i) {
        xgw_session_t *session = &dp->sessions.sessions[i];
        if (!session->active) {
            continue;
        }
        if (session->control_state != XGW_CTRL_ESTABLISHED) {
            continue;
        }
        if (session->remote_addr.host[0] == '\0' || session->remote_addr.port == 0U) {
            continue;
        }
        return session;
    }
    return NULL;
}

static int is_forwarder(xgw_role_t role) {
    return role == XGW_ROLE_INGRESS || role == XGW_ROLE_RELAY;
}

static int obfs_hop_mode(const xgw_runtime_config_t *config) {
    return strcmp(config->obfs_scope, "hop") == 0 || config->obfs_scope[0] == '\0';
}

static const xgw_pool_node_t *select_pool_node(const xgw_dataplane_t *dp) {
    return xgw_pool_select_best(&dp->pool);
}

static void maybe_pace_session(xgw_traffic_limiter_t *traffic_limiter, xgw_session_t *session, size_t bytes) {
    uint64_t now_ns;
    uint64_t wait_ns;
    unsigned int attempts = 0U;
    if (session == NULL) {
        traffic_limiter_consume(traffic_limiter, bytes);
        return;
    }
    while (!xgw_cc_can_send(&session->cc, bytes) && attempts < 32U) {
        xgw_sleep_us(250ULL);
        attempts++;
    }
    if (attempts > 0U) {
        p0_note_cc_stall((uint64_t) attempts * 250ULL, 1);
    }
    now_ns = monotonic_ns();
    wait_ns = xgw_cc_next_send_delay_ns(&session->cc, bytes, now_ns);
    if (wait_ns > 0U) {
        g_p0_prev.pace_wait_us += (wait_ns + 999ULL) / 1000ULL;
        xgw_sleep_us((wait_ns + 999ULL) / 1000ULL);
        now_ns = monotonic_ns();
    }
    session->cc.next_send_time_ns = max_u64(session->cc.next_send_time_ns, now_ns) +
                                    xgw_cc_pacing_delay_ns(&session->cc, bytes);
    traffic_limiter_consume(traffic_limiter, bytes);
}

static void maybe_pace_session_priority(xgw_traffic_limiter_t *traffic_limiter,
                                        xgw_session_t *session,
                                        size_t bytes,
                                        uint32_t priority) {
    if (priority >= XGW_FLOW_PRIORITY_HIGH) {
        uint64_t now_ns;
        if (session == NULL) {
            traffic_limiter_consume(traffic_limiter, bytes);
            return;
        }
        while (!xgw_cc_can_send(&session->cc, bytes)) {
            if (session->cc.inflight_bytes <= session->cc.cwnd_bytes / 2ULL) {
                break;
            }
            xgw_sleep_us(100ULL);
            p0_note_cc_stall(100ULL, 1);
            break;
        }
        now_ns = monotonic_ns();
        if (session->cc.next_send_time_ns > now_ns) {
            uint64_t wait_ns = session->cc.next_send_time_ns - now_ns;
            if (wait_ns > 500000ULL) {
                wait_ns = 500000ULL;
            }
            xgw_sleep_us((wait_ns + 999ULL) / 1000ULL);
            now_ns = monotonic_ns();
        }
        session->cc.next_send_time_ns = max_u64(session->cc.next_send_time_ns, now_ns) +
                                        xgw_cc_pacing_delay_ns(&session->cc, bytes) / 2ULL;
        traffic_limiter_consume(traffic_limiter, bytes);
        return;
    }
    maybe_pace_session(traffic_limiter, session, bytes);
}

/* per-stream 窗口门控 + slot 级 pacing。
 * 窗口:用 xport 自己的 inflight vs cwnd 配额(消除 HOL,一条慢 stream 不卡同 slot 其他 stream)。
 * pacing:仍用 session->cc(slot 共享带宽估计/速率),保证拥塞域级别的发送节奏。
 * xport==NULL(控制帧/未知 stream)回退到 session 级门控,保持兼容。 */
static void maybe_pace_stream(xgw_traffic_limiter_t *traffic_limiter,
                              xgw_session_t *session,
                              xgw_stream_xport_t *xport,
                              uint64_t stream_quota,
                              size_t bytes,
                              uint32_t priority) {
    uint64_t now_ns;
    unsigned int attempts = 0U;
    if (session == NULL) {
        traffic_limiter_consume(traffic_limiter, bytes);
        return;
    }
    if (xport == NULL) {
        /* 无 per-stream 上下文:退回原 session 级 pacing(含优先级快路径)。 */
        maybe_pace_session_priority(traffic_limiter, session, bytes, priority);
        return;
    }
    /* per-stream inflight 老化:xport->inflight 是窗口门控用的近似在途计数(精确拥塞控制
     * 由 session->cc 的 session 级 ACK 负责)。按"距上次活动的时长 × slot pacing 速率"
     * 估算这段时间内本 stream 应已发走/被确认的量,从 inflight 扣减。这样:
     *   - 活跃流持续发 → inflight 累积到配额上限被门控(只卡自己);
     *   - 流慢下来不发 → inflight 随时间释放,不永久占配额、不会只增不减死锁。
     * 无需 ACK 帧带 stream_id,避免大改反馈链。 */
    if (xport->inflight_bytes > 0U && xport->last_activity_us != 0U) {
        uint64_t now = now_us();
        if (now > xport->last_activity_us) {
            uint64_t elapsed_us = now - xport->last_activity_us;
            uint64_t rate_bps = session->cc.pacing_rate_bps > 0U ? session->cc.pacing_rate_bps : 8000000ULL;
            uint64_t drained = (rate_bps / 8ULL) * elapsed_us / 1000000ULL;
            xport->inflight_bytes = drained >= xport->inflight_bytes ? 0U : xport->inflight_bytes - drained;
        }
    }
    /* per-stream 窗口门控:本 stream 占满自己配额则自旋等待(只卡自己),
     * 高优先级流给更短的自旋上限以降低延迟。 */
    {
        unsigned int max_attempts = priority >= XGW_FLOW_PRIORITY_HIGH ? 8U : 32U;
        while (!xgw_cc_stream_can_send(&session->cc, xport->inflight_bytes, stream_quota, bytes) &&
               attempts < max_attempts) {
            xgw_sleep_us(250ULL);
            attempts++;
        }
    }
    if (attempts > 0U) {
        p0_note_cc_stall((uint64_t) attempts * 250ULL, 1);
    }
    /* slot 级 pacing:共享 cc 的发送节奏(带宽整形),与窗口门控解耦。 */
    now_ns = monotonic_ns();
    if (session->cc.next_send_time_ns > now_ns) {
        uint64_t wait_ns = session->cc.next_send_time_ns - now_ns;
        if (wait_ns > 500000ULL) {
            wait_ns = 500000ULL;
        }
        g_p0_prev.pace_wait_us += (wait_ns + 999ULL) / 1000ULL;
        xgw_sleep_us((wait_ns + 999ULL) / 1000ULL);
        now_ns = monotonic_ns();
    }
    session->cc.next_send_time_ns = max_u64(session->cc.next_send_time_ns, now_ns) +
                                    xgw_cc_pacing_delay_ns(&session->cc, bytes);
    traffic_limiter_consume(traffic_limiter, bytes);
}

static int udp_send_one(xgw_udp_socket_t *udp,
                        const xgw_obfs_t *obfs,
                        int hop_obfs,
                        const char *host,
                        uint16_t port,
                        const uint8_t *frame,
                        size_t frame_len) {
    uint8_t obfs_buf[XGW_MAX_FRAME_SIZE + XGW_OBFS_SALT_LEN];
    const uint8_t *payload = frame;
    size_t payload_len = frame_len;
    if (xgw_obfs_enabled(obfs) && hop_obfs) {
        payload_len = xgw_obfs_encode(obfs, frame, frame_len, obfs_buf, sizeof(obfs_buf));
        payload = obfs_buf;
    }
    return xgw_udp_send(udp, payload, payload_len, host, port);
}

static int outbound_send_one(xgw_outbound_t *outbound,
                             const xgw_obfs_t *obfs,
                             int hop_obfs,
                             const uint8_t *frame,
                             size_t frame_len) {
    uint8_t obfs_buf[XGW_MAX_FRAME_SIZE + XGW_OBFS_SALT_LEN];
    const uint8_t *payload = frame;
    size_t payload_len = frame_len;
    if (xgw_obfs_enabled(obfs) && hop_obfs) {
        payload_len = xgw_obfs_encode(obfs, frame, frame_len, obfs_buf, sizeof(obfs_buf));
        payload = obfs_buf;
    }
    return xgw_outbound_send(outbound, payload, payload_len, outbound->current_target_host, outbound->current_target_port);
}

static int send_frame_series_udp(xgw_udp_socket_t *udp,
                                 xgw_traffic_limiter_t *traffic_limiter,
                                 xgw_session_t *session,
                                 xgw_stream_xport_pool_t *streams,
                                 const xgw_runtime_config_t *config,
                                 const xgw_obfs_t *obfs,
                                 int hop_obfs,
                                 const char *host,
                                 uint16_t port,
                                 const xgw_frame_batch_t *batch,
                                 const xgw_flow_hint_t *hint) {
    size_t i;
    char safe_host[256];
    const char *send_host = host;
    safe_host[0] = '\0';
    if (host != NULL) {
        snprintf(safe_host, sizeof(safe_host), "%s", host);
        send_host = safe_host;
    }
    uint32_t priority = hint != NULL && hint->valid && hint->priority > 0U
                        ? (uint32_t) hint->priority
                        : classify_target_priority_runtime(send_host, port);
    uint32_t copies = config == NULL ? 1U : config->profile.redundant_copies;
    if (copies == 0U) {
        copies = 1U;
    }
    if (hint != NULL && hint->valid && hint->preferred_copies > 0U) {
        copies = hint->preferred_copies;
    }
    if (session != NULL && session->cc.mode == XGW_CC_BRUTAL && copies < 2U) {
        copies = 2U;
    }
    if (priority >= XGW_FLOW_PRIORITY_HIGH) {
        copies = 1U;
    }
    if (XGW_RT_VERBOSE()) printf("runtime.udp.send target=%s:%u frames=%zu copies=%u session_peer=%s:%u state=%d\n",
           send_host == NULL ? "" : send_host,
           port,
           batch == NULL ? 0U : batch->frame_count,
           copies,
           session == NULL ? "" : session->remote_addr.host,
           session == NULL ? 0U : session->remote_addr.port,
           session == NULL ? -1 : (int) session->control_state);
    fflush(stdout);
    for (i = 0; i < batch->frame_count; ++i) {
        uint32_t c;
        xgw_header_t header;
        const uint8_t *payload = NULL;
        size_t payload_len = 0U;
        int recorded = 0;
        xgw_stream_xport_t *tx_xport = NULL;
        uint64_t stream_quota = 0U;
        if (!xgw_header_decode(batch->frames[i], batch->frame_lengths[i], &header, &payload, &payload_len)) {
            return 0;
        }
        /* per-stream 窗口门控:为该帧所属业务流取传输状态,设一个"防单流独占"的配额上限。
         * 注意:配额不能用 cwnd/活跃流数 —— 高并发(TikTok 一会话上百条流)下会塌缩到几 KB,
         * 把每条流限死在 1~2 帧/RTT,导致 first_byte 尾延迟爆炸(实测 p90 6s、19% 超时)。
         * HOL 已由 per-stream 序号空间 + 总 cwnd 门控(物理上限)+ DRR 调度 + inflight 老化共同消除,
         * 此配额只需防"单流独占整个 cwnd"即可,故取 cwnd/min(active,2):
         *   - 单流时 = 整个 cwnd(work-conserving,不浪费带宽);
         *   - 多流时 = cwnd/2 上限(任一流最多占一半,留一半给其他流轮转),不随流数塌缩。 */
        if (streams != NULL && session != NULL && header.stream_id != 0U) {
            tx_xport = xgw_stream_xport_get(streams, session->id, header.stream_id, 1);
            if (tx_xport != NULL && session->cc.cwnd_bytes > 0U) {
                size_t active = xgw_stream_xport_active_count(streams, session->id, now_us(), 200000ULL);
                uint64_t divisor = active < 2U ? (uint64_t) (active == 0U ? 1U : active) : 2U;
                stream_quota = session->cc.cwnd_bytes / divisor;
                /* 保底:配额不低于单帧。 */
                if (stream_quota < batch->frame_lengths[i]) {
                    stream_quota = batch->frame_lengths[i];
                }
            }
        }
        for (c = 0U; c < copies; ++c) {
            int sent_bytes = -1;
            int send_error = 0;
            maybe_pace_stream(traffic_limiter, session, tx_xport, stream_quota, batch->frame_lengths[i], priority);
            if (XGW_RT_VERBOSE() && config != NULL &&
                (config->role == XGW_ROLE_RELAY || config->role == XGW_ROLE_INGRESS)) {
                printf("runtime.udp.final_send.prepare role=%s target=%s:%u frame_index=%zu/%zu copy=%u/%u frame_len=%zu\n",
                       xgw_role_name(config->role),
                       send_host == NULL ? "" : send_host,
                       port,
                       i + 1U,
                       batch->frame_count,
                       c + 1U,
                       copies,
                       batch->frame_lengths[i]);
                fflush(stdout);
            }
            {
                uint8_t obfs_buf[XGW_MAX_FRAME_SIZE + XGW_OBFS_SALT_LEN];
                const uint8_t *payload = batch->frames[i];
                size_t payload_len = batch->frame_lengths[i];
                if (xgw_obfs_enabled(obfs) && hop_obfs) {
                    payload_len = xgw_obfs_encode(obfs, batch->frames[i], batch->frame_lengths[i], obfs_buf, sizeof(obfs_buf));
                    payload = obfs_buf;
                }
                if (!xgw_udp_send_ex(udp, payload, payload_len, send_host, port, &sent_bytes, &send_error)) {
                    if (xgw_udp_error_is_too_big(send_error)) {
                        g_pmtu_too_big_pending = 1;
                        printf("pmtu.too_big target=%s:%u payload_len=%zu err=%d\n",
                               send_host == NULL ? "" : send_host, port, payload_len, send_error);
                        fflush(stdout);
                    }
                    if (XGW_RT_VERBOSE() && config != NULL &&
                        (config->role == XGW_ROLE_RELAY || config->role == XGW_ROLE_INGRESS)) {
                        printf("runtime.udp.final_send.result role=%s target=%s:%u frame_index=%zu/%zu copy=%u/%u payload_len=%zu sent=%d err=%d ok=0\n",
                               xgw_role_name(config->role),
                               send_host == NULL ? "" : send_host,
                               port,
                               i + 1U,
                               batch->frame_count,
                               c + 1U,
                               copies,
                               payload_len,
                               sent_bytes,
                               send_error);
                        fflush(stdout);
                    }
                    return 0;
                }
                if (XGW_RT_VERBOSE() && config != NULL &&
                    (config->role == XGW_ROLE_RELAY || config->role == XGW_ROLE_INGRESS)) {
                    printf("runtime.udp.final_send.result role=%s target=%s:%u frame_index=%zu/%zu copy=%u/%u payload_len=%zu sent=%d err=%d ok=1\n",
                           xgw_role_name(config->role),
                           send_host == NULL ? "" : send_host,
                           port,
                           i + 1U,
                           batch->frame_count,
                           c + 1U,
                           copies,
                           payload_len,
                           sent_bytes,
                           send_error);
                    fflush(stdout);
                }
            }
            if (!recorded) {
                xgw_session_record_send(session, header.sequence, batch->frame_lengths[i], now_us());
                /* per-stream 在途记账:窗口门控据此判断本 stream 是否占满配额。
                 * 递减由 ACK 路径按 stream_id 命中对应 xport 完成(见 dataplane 接收侧)。 */
                if (tx_xport != NULL) {
                    tx_xport->inflight_bytes += batch->frame_lengths[i];
                    tx_xport->last_activity_us = now_us();
                }
                /* 回程时间线埋点:本帧真正 sendto 网络的时刻(batch 首帧打一次)。
                 * 与同机 emit_return 对比 → 入 stream_sched 队列到真正发出的排队耗时(定位 3.3s 是否卡调度)。 */
                if (i == 0U && header.stream_id != 0U) {
                    log4c_info("rtt.egress.actual_send stream=%u seq=%llu frames=%zu",
                               header.stream_id, (unsigned long long) header.sequence, batch->frame_count);
                }
                recorded = 1;
            }
        }
    }
    return 1;
}

static int send_frame_series_outbound(xgw_outbound_t *outbound,
                                      xgw_traffic_limiter_t *traffic_limiter,
                                      xgw_session_t *session,
                                      const xgw_obfs_t *obfs,
                                      int hop_obfs,
                                      const xgw_frame_batch_t *batch,
                                      const xgw_flow_hint_t *hint) {
    size_t i;
    uint32_t copies = session->cc.mode == XGW_CC_BRUTAL ? 2U : 1U;
    uint32_t priority = hint != NULL && hint->valid && hint->priority > 0U ? (uint32_t) hint->priority : XGW_FLOW_PRIORITY_NORMAL;
    if (hint != NULL && hint->valid && hint->preferred_copies > 0U) {
        copies = hint->preferred_copies;
    }
    if (priority >= XGW_FLOW_PRIORITY_HIGH) {
        copies = 1U;
    }
    for (i = 0; i < batch->frame_count; ++i) {
        uint32_t c;
        xgw_header_t header;
        const uint8_t *payload = NULL;
        size_t payload_len = 0U;
        int recorded = 0;
        if (!xgw_header_decode(batch->frames[i], batch->frame_lengths[i], &header, &payload, &payload_len)) {
            return 0;
        }
        for (c = 0U; c < copies; ++c) {
            maybe_pace_session_priority(traffic_limiter, session, batch->frame_lengths[i], priority);
            if (!outbound_send_one(outbound, obfs, hop_obfs, batch->frames[i], batch->frame_lengths[i])) {
                return 0;
            }
            if (!recorded) {
                xgw_session_record_send(session, header.sequence, batch->frame_lengths[i], now_us());
                recorded = 1;
            }
        }
    }
    return 1;
}

static int send_control_frame_udp(xgw_udp_socket_t *udp,
                                  xgw_session_t *session,
                                  const xgw_obfs_t *obfs,
                                  int hop_obfs,
                                  const char *host,
                                  uint16_t port,
                                  const uint8_t *frame,
                                  size_t frame_len) {
    xgw_header_t header;
    const uint8_t *payload = NULL;
    size_t payload_len = 0U;
    if (frame == NULL || frame_len == 0U) {
        return 1;
    }
    if (!xgw_header_decode(frame, frame_len, &header, &payload, &payload_len)) {
        return 0;
    }
    xgw_session_record_send(session, header.sequence, frame_len, now_us());
    if (XGW_RT_VERBOSE()) {
    printf("control.tx type=%s target=%s:%u id=%u state=%d frame_len=%zu\n",
           xgw_message_type_name(header.type),
           host == NULL ? "" : host,
           port,
           session == NULL ? 0U : session->id,
           session == NULL ? -1 : (int) session->control_state,
           frame_len);
    fflush(stdout);
    }
    return udp_send_one(udp, obfs, hop_obfs, host, port, frame, frame_len);
}

static int maybe_send_session_feedback(xgw_udp_socket_t *udp,
                                       xgw_session_t *session,
                                       const xgw_obfs_t *obfs,
                                       int hop_obfs,
                                       const char *host,
                                       uint16_t port,
                                       uint64_t timestamp_us) {
    xgw_ack_info_t ack;
    xgw_keepalive_payload_t feedback;
    xgw_header_t header;
    uint8_t payload[64];
    uint8_t frame[XGW_MAX_FRAME_SIZE];
    size_t payload_len;
    size_t frame_len;
    if (session == NULL || host == NULL || host[0] == '\0' || port == 0U) {
        return 1;
    }
    if (session->control_state != XGW_CTRL_ESTABLISHED) {
        return 1;
    }
    if (!xgw_session_flush_feedback(session, timestamp_us, &ack)) {
        return 1;
    }
    xgw_control_make_ack(&ack, timestamp_us, &feedback);
    payload_len = xgw_encode_keepalive_payload(&feedback, payload, sizeof(payload));
    if (payload_len == 0U) {
        return 0;
    }
    memset(&header, 0, sizeof(header));
    header.version = 1U;
    header.type = XGW_MESSAGE_ACK;
    header.flags = XGW_FLAG_CONTROL;
    header.session_id = session->id;
    header.sequence = session->next_tx_seq++;
    header.payload_length = (uint16_t) payload_len;
    if (!xgw_header_encode(&header, payload, payload_len, frame, sizeof(frame), &frame_len)) {
        return 0;
    }
    return send_control_frame_udp(udp, session, obfs, hop_obfs, host, port, frame, frame_len);
}

static int maybe_send_keepalive(xgw_udp_socket_t *udp,
                                xgw_session_t *session,
                                const xgw_runtime_config_t *config,
                                const xgw_obfs_t *obfs,
                                int hop_obfs,
                                const char *host,
                                uint16_t port) {
    xgw_keepalive_payload_t keepalive;
    xgw_header_t header;
    uint8_t payload[64];
    uint8_t frame[XGW_MAX_FRAME_SIZE];
    size_t payload_len;
    size_t frame_len;
    uint64_t now = now_us();
    uint32_t keepalive_sec;
    xgw_ack_info_t ack;
    int has_ack = 0;
    if (config->keepalive_sec == 0U || session->control_state != XGW_CTRL_ESTABLISHED) {
        return 1;
    }
    keepalive_sec = config->keepalive_sec;
    if (session->scope.slot == XGW_SESSION_SLOT_CONTROL && keepalive_sec > 3U) {
        keepalive_sec = 3U;
    } else if (session->scope.slot == XGW_SESSION_SLOT_MEDIA && keepalive_sec > 5U) {
        keepalive_sec = 5U;
    }
    if (session->last_keepalive_us != 0U &&
        now - session->last_keepalive_us < (uint64_t) keepalive_sec * 1000000ULL) {
        return 1;
    }
    memset(&ack, 0, sizeof(ack));
    has_ack = xgw_session_flush_feedback(session, now, &ack);
    xgw_control_make_keepalive(has_ack ? &ack : NULL, now, &keepalive);
    payload_len = xgw_encode_keepalive_payload(&keepalive, payload, sizeof(payload));
    if (payload_len == 0U) {
        return 0;
    }
    memset(&header, 0, sizeof(header));
    header.version = 1U;
    header.type = XGW_MESSAGE_KEEPALIVE;
    header.flags = XGW_FLAG_CONTROL;
    header.session_id = session->id;
    header.sequence = session->next_tx_seq++;
    header.payload_length = (uint16_t) payload_len;
    if (!xgw_header_encode(&header, payload, payload_len, frame, sizeof(frame), &frame_len)) {
        return 0;
    }
    if (!send_control_frame_udp(udp, session, obfs, hop_obfs, host, port, frame, frame_len)) {
        return 0;
    }
    session->last_keepalive_us = now;
    return 1;
}

static void maybe_refresh_control_session(xgw_session_t *session,
                                          const xgw_runtime_config_t *config,
                                          const char *host,
                                          uint16_t port) {
    uint64_t now;
    uint64_t idle_us = 0U;
    uint64_t idle_limit_us;
    if (session == NULL || config == NULL || host == NULL || host[0] == '\0' || port == 0U) {
        return;
    }
    if (config->auth_token[0] == '\0' || session->control_state != XGW_CTRL_ESTABLISHED) {
        return;
    }
    if (config->max_idle_timeout_sec == 0U) {
        return;
    }
    now = now_us();
    if (session->last_control_rx_us > 0U && now > session->last_control_rx_us) {
        idle_us = now - session->last_control_rx_us;
    } else if (session->last_keepalive_us > 0U && now > session->last_keepalive_us) {
        idle_us = now - session->last_keepalive_us;
    } else if (session->last_control_tx_us > 0U && now > session->last_control_tx_us) {
        idle_us = now - session->last_control_tx_us;
    }
    idle_limit_us = (uint64_t) config->max_idle_timeout_sec * 1000000ULL;
    if (idle_limit_us > 2000000ULL) {
        idle_limit_us -= 2000000ULL;
    }
    if (idle_us < idle_limit_us) {
        return;
    }
    /* 仅在长时间彻底无控制面往返时，作为最后手段重置并重握手。
     * 闲置阈值已提到 max_idle_timeout_sec（默认 120s，对齐前端 QUIC MaxIdleTimeout），
     * keepalive（3-10s/slot）正常时不会走到这里；前端长闲置首连不再依赖此路径。 */
    printf("control.session.refresh remote=%s:%u id=%u idle_ms=%llu limit_ms=%llu state=%d outstanding=%zu inflight=%llu reason=idle_guard\n",
           host,
           port,
           session->id,
           (unsigned long long) (idle_us / 1000ULL),
           (unsigned long long) (idle_limit_us / 1000ULL),
           (int) session->control_state,
           session->outstanding_count,
           (unsigned long long) session->cc.inflight_bytes);
    fflush(stdout);
    xgw_session_reset_transport_state(session);
    session->control_state = XGW_CTRL_INIT;
    session->security.session_ready = 0;
    session->security.local_nonce = 0U;
    session->security.peer_nonce = 0U;
}

static int maybe_send_handshake(xgw_udp_socket_t *udp,
                                xgw_session_t *session,
                                const xgw_runtime_config_t *config,
                                const xgw_negotiation_info_t *negotiation,
                                const xgw_obfs_t *obfs,
                                int hop_obfs,
                                const char *host,
                                uint16_t port) {
    xgw_hello_payload_t hello;
    xgw_header_t header;
    uint8_t payload[64];
    uint8_t frame[XGW_MAX_FRAME_SIZE];
    size_t payload_len;
    size_t frame_len;
    uint64_t now = now_us();
    if (config->auth_token[0] == '\0' ||
        (session->control_state != XGW_CTRL_INIT && session->control_state != XGW_CTRL_HELLO_SENT)) {
        return 1;
    }
    if (session->control_state == XGW_CTRL_HELLO_SENT &&
        session->last_control_tx_us != 0U &&
        now - session->last_control_tx_us < 1000000ULL) {
        return 1;
    }
    if (session->security.local_nonce == 0U) {
        xgw_security_begin_handshake(&session->security, now ^ ((uint64_t) session->id << 16U));
    }
    xgw_control_make_hello(&session->security, negotiation, config->profile.mtu, &hello);
    payload_len = xgw_encode_hello_payload(&hello, payload, sizeof(payload));
    if (payload_len == 0U) {
        return 0;
    }
    memset(&header, 0, sizeof(header));
    header.version = 1U;
    header.type = XGW_MESSAGE_HELLO;
    header.flags = XGW_FLAG_CONTROL;
    header.session_id = session->id;
    header.sequence = session->next_tx_seq++;
    header.payload_length = (uint16_t) payload_len;
    if (!xgw_header_encode(&header, payload, payload_len, frame, sizeof(frame), &frame_len)) {
        return 0;
    }
    if (!send_control_frame_udp(udp, session, obfs, hop_obfs, host, port, frame, frame_len)) {
        return 0;
    }
    session->control_state = XGW_CTRL_HELLO_SENT;
    session->last_control_tx_us = now;
    printf("control.hello.tx remote=%s:%u id=%u state=%d nonce=%llu\n",
           host == NULL ? "" : host,
           port,
           session->id,
           session->control_state,
           (unsigned long long) session->security.local_nonce);
    fflush(stdout);
    return 1;
}

static int flush_pending_feedbacks_udp(xgw_udp_socket_t *udp,
                                       xgw_dataplane_t *dp,
                                       const xgw_obfs_t *obfs,
                                       int hop_obfs) {
    size_t i;
    uint64_t now = now_us();
    if (udp == NULL || dp == NULL) {
        return 1;
    }
    for (i = 0; i < dp->sessions.count; ++i) {
        xgw_session_t *session = &dp->sessions.sessions[i];
        if (!session->active ||
            session->remote_addr.host[0] == '\0' ||
            session->remote_addr.port == 0U) {
            continue;
        }
        if (!maybe_send_session_feedback(udp,
                                         session,
                                         obfs,
                                         hop_obfs,
                                         session->remote_addr.host,
                                         session->remote_addr.port,
                                         now)) {
            return 0;
        }
    }
    return 1;
}

/* 调度器聚合自检：随机构造活跃流，对拍 O(N) 快路径与 _ref，逐值等价返回 0。 */
int xgw_runtime_sched_selftest(char *error, size_t error_len) {
    xgw_stream_scheduler_t *sched = (xgw_stream_scheduler_t *) calloc(1, sizeof(*sched));
    unsigned seed = 0x12345u;
    int trial;
    if (sched == NULL) {
        if (error != NULL) snprintf(error, error_len, "sched selftest alloc failed");
        return 1;
    }
    for (trial = 0; trial < 40; ++trial) {
        size_t n = (size_t) (1u + (seed = seed * 1103515245u + 12345u, (seed >> 16) % 200u));
        size_t i;
        sched->active_n = 0U;
        for (i = 0U; i < XGW_STREAM_SCHED_CAP; ++i) sched->entries[i].active = 0;
        for (i = 0U; i < n && i < XGW_STREAM_SCHED_CAP; ++i) {
            xgw_stream_sched_entry_t *e = &sched->entries[i];
            unsigned r = (seed = seed * 1103515245u + 12345u, seed >> 8);
            e->active = 1;
            e->slot_pos = sched->active_n;
            sched->active_idx[sched->active_n++] = i;
            e->pending = (r & 1u) ? 1U : 0U;
            e->inflight_bytes = (uint64_t) ((r >> 1) % 5000u);
            e->send_direction = (xgw_channel_direction_t) ((r >> 4) % 3u); /* 含 NONE */
            e->session_slot = (xgw_session_slot_t) ((r >> 6) % 3u);
            e->hint.valid = 1U;
            e->hint.priority = (uint8_t) ((r >> 8) % 5u);
            e->hint.flow_class = (uint8_t) ((r >> 11) % 9u);
            /* 小基数 fsid/route/line + 部分置空，制造大量分组重叠与空字段 */
            if ((r >> 14) % 4u == 0u) e->front_session_id[0] = '\0';
            else snprintf(e->front_session_id, sizeof(e->front_session_id), "fs%u", (r >> 15) % 6u);
            if ((r >> 18) % 5u == 0u) e->route_name[0] = '\0';
            else snprintf(e->route_name, sizeof(e->route_name), "rt%u", (r >> 19) % 5u);
            if ((r >> 22) % 5u == 0u) e->line_id[0] = '\0';
            else snprintf(e->line_id, sizeof(e->line_id), "ln%u", (r >> 23) % 4u);
        }
        stream_sched_agg_rebuild(sched);
        for (i = 0U; i < sched->active_n; ++i) {
            const xgw_stream_sched_entry_t *e = &sched->entries[sched->active_idx[i]];
            uint32_t pc_fast = (uint32_t) stream_sched_group_query(sched, e).cnt;
            uint32_t pc_ref = stream_sched_group_pending_count_ref(sched, e);
            uint32_t tw_fast = stream_sched_total_weight(sched, e->line_id, e->send_direction);
            uint32_t tw_ref = stream_sched_total_weight_ref(sched, e->line_id, e->send_direction);
            if (pc_fast != pc_ref || tw_fast != tw_ref) {
                if (error != NULL) snprintf(error, error_len,
                    "sched mismatch trial=%d n=%zu i=%zu pc=%u/%u tw=%u/%u",
                    trial, n, i, pc_fast, pc_ref, tw_fast, tw_ref);
                free(sched);
                return 1;
            }
        }
    }
    free(sched);
    return 0;
}

/* burst 回程出队的回调上下文：承载主循环的逐帧 enqueue 所需的全部运行时状态。
 * 取代旧的"单次 dequeue + 内联 enqueue"，使一次 tick 内可连续吐多帧。 */
typedef struct runtime_bridge_egress_ctx {
    const xgw_runtime_config_t *config;
    xgw_dataplane_t *dp;
    xgw_session_t *session;
    xgw_runtime_channels_t *runtime_channels;
    const xgw_line_runtime_t *active_line;
    const xgw_node_hops_t *active_hops;
    xgw_bridge_egress_t *bridge_egress;
    xgw_bridge_target_entry_t *bridge_targets;
    xgw_stream_scheduler_t *stream_sched;
    xgw_frame_batch_t *batch;
    uint8_t *fec_frame;
    size_t fec_len;
    char *error;
    size_t error_len;
    int did_work;        /* 出参：本次 burst 是否产生了工作。 */
} runtime_bridge_egress_ctx_t;

/* 单帧回程 enqueue 回调：与旧内联逻辑逐句等价。
 * 返回 0=继续 burst（含 channel_not_ready / sched_drop 软跳过），-1=致命错误（中止 burst→break 主循环）。 */
static int runtime_bridge_egress_emit(void *vctx,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      const char *upstream_host,
                                      uint16_t upstream_port) {
    runtime_bridge_egress_ctx_t *ctx = (runtime_bridge_egress_ctx_t *) vctx;
    char prev_host[64];
    uint16_t prev_port;
    xgw_session_t *send_session = NULL;
    xgw_bridge_target_entry_t *hint = NULL;
    xgw_flow_hint_t flow_hint;
    xgw_session_slot_t slot = XGW_SESSION_SLOT_BULK;

    if (payload == NULL || payload_len == 0U) {
        return 0;
    }
    /* 回程时间线埋点:egress 把回程数据帧交给发送层(入 stream_sched)的时刻。
     * 与 bridge 的"上游首字节"对比 → egress 是否持有;与 ingress"收到回程帧"对比 → 传输耗时。 */
    {
        uint32_t rtt_sid = 0U;
        if (parse_stream_id_from_bridge_payload(payload, payload_len, &rtt_sid) && rtt_sid != 0U) {
            log4c_info("rtt.egress.emit_return stream=%u len=%zu", rtt_sid, payload_len);
        }
    }
    snprintf(prev_host, sizeof(prev_host), "%s", upstream_host == NULL ? "" : upstream_host);
    prev_port = upstream_port;
    memset(&flow_hint, 0, sizeof(flow_hint));

    if (XGW_RT_VERBOSE()) {
        printf("runtime.bridge_egress.dequeue len=%zu\n", payload_len);
        fflush(stdout);
    }
    if (prev_host[0] == '\0' || prev_port == 0U) {
        (void) xgw_bridge_egress_get_upstream(ctx->bridge_egress, prev_host, sizeof(prev_host), &prev_port);
    }
    if (prev_host[0] == '\0' || prev_port == 0U) {
        if (ctx->config->role == XGW_ROLE_EGRESS &&
            ctx->session->control_state == XGW_CTRL_ESTABLISHED &&
            ctx->session->remote_addr.host[0] != '\0' &&
            ctx->session->remote_addr.port != 0U) {
            snprintf(prev_host, sizeof(prev_host), "%s", ctx->session->remote_addr.host);
            prev_port = ctx->session->remote_addr.port;
        } else if (!runtime_endpoint_addr(ctx->active_hops, ctx->active_hops->previous, prev_host, sizeof(prev_host), &prev_port)) {
            set_error(ctx->error, ctx->error_len, "invalid previous hop address");
            return -1;
        }
    }
    if (prev_host[0] == '\0' || prev_port == 0U) {
        set_error(ctx->error, ctx->error_len, "invalid previous hop address");
        return -1;
    }
    hint = find_bridge_target_from_payload(ctx->bridge_targets, payload, payload_len);
    fill_flow_hint_from_target(hint, &flow_hint);
    if (flow_hint.valid) {
        slot = xgw_flow_class_to_slot(flow_hint.flow_class);
    }
    send_session = runtime_channel_session(ctx->runtime_channels,
                                           runtime_line_id(ctx->active_line),
                                           XGW_CHANNEL_PREVIOUS,
                                           slot,
                                           1);
    if (send_session == NULL) {
        printf("runtime.bridge_egress.channel_not_ready target=%s:%u slot=%s\n",
               prev_host,
               prev_port,
               runtime_session_slot_name(slot));
        fflush(stdout);
        ctx->did_work = 1;
        return 0; /* 软跳过：通道未就绪，丢弃该帧，继续 burst 其它帧。 */
    }
    log_session_summary("runtime.bridge_egress.session", send_session);
    if (XGW_RT_VERBOSE()) {
        printf("runtime.bridge_egress.return target=%s:%u state=%d peer=%s:%u payload=%zu\n",
               prev_host, prev_port,
               send_session->control_state,
               send_session->remote_addr.host,
               send_session->remote_addr.port,
               payload_len);
        fflush(stdout);
    }
    xgw_dataplane_set_flow_hint(ctx->dp, hint == NULL ? NULL : &flow_hint);
    if (!xgw_dataplane_build_outbound(ctx->dp, send_session, payload, payload_len, ctx->batch, ctx->fec_frame, XGW_MAX_FRAME_SIZE, &ctx->fec_len)) {
        set_error(ctx->error, ctx->error_len, xgw_dataplane_last_error());
        return -1;
    }
    if (strcmp(ctx->config->transport, "udp") == 0) {
        if (!stream_sched_enqueue(ctx->stream_sched,
                                  payload,
                                  payload_len,
                                  runtime_line_id(ctx->active_line),
                                  XGW_CHANNEL_PREVIOUS,
                                  hint)) {
            printf("runtime.warn stream_sched_drop stage=bridge_egress_return stream_active=%zu target=%s:%u payload=%zu\n",
                   stream_sched_active_count(ctx->stream_sched),
                   prev_host,
                   prev_port,
                   payload_len);
            fflush(stdout);
            forget_bridge_target_if_close(ctx->bridge_targets, payload, payload_len);
            forget_xport_if_close(&ctx->dp->streams, payload, payload_len);
            ctx->did_work = 1;
            return 0; /* 软跳过：调度器满，丢弃该帧，继续 burst。 */
        }
        ctx->did_work = 1;
    }
    return 0;
}

int xgw_runtime_run(const xgw_runtime_config_t *config, char *error, size_t error_len) {
    xgw_route_manager_t routes;
    const xgw_line_runtime_t *active_line = NULL;
    const xgw_node_hops_t *active_hops = NULL;
    xgw_dataplane_t *dp = NULL;
    xgw_tun_device_t tun;
    xgw_udp_socket_t udp;
    xgw_afxdp_socket_t xsk;
    xgw_outbound_t outbound;
    xgw_bridge_server_t *bridge = NULL;
    xgw_bridge_egress_t *bridge_egress = NULL;
    xgw_obfs_t obfs;
    xgw_traffic_limiter_t traffic_limiter;
    xgw_session_t *session;
    xgw_frame_batch_t *batch = NULL;
    xgw_reassembly_result_t *result = NULL;
    uint8_t *fec_frame = NULL;
    uint8_t *tun_buf = NULL;
    size_t tun_len = 0U;
    size_t fec_len = 0U;
    xgw_packet_t *packet = NULL;
    uint8_t *bridge_buf = NULL;
    size_t bridge_len = 0U;
    xgw_bridge_target_entry_t *bridge_targets = NULL;
    xgw_stream_scheduler_t *stream_sched = NULL;
    xgw_runtime_channels_t runtime_channels;
    int tun_opened = 0;
    int net_opened = 0;
    int hop_obfs = 0;
    char host[64];
    uint16_t port = 0U;
    char control_peer_host[64];
    uint16_t control_peer_port = 0U;
    const xgw_endpoint_t *control_peer = NULL;
    time_t last_route_metrics_dump = 0;
    time_t last_cc_dump = 0;
    time_t last_egress_burst_dump = 0;

    memset(&tun, 0, sizeof(tun));
    memset(&udp, 0, sizeof(udp));
    memset(&xsk, 0, sizeof(xsk));
    memset(&outbound, 0, sizeof(outbound));
    memset(&runtime_channels, 0, sizeof(runtime_channels));
    xgw_obfs_init(&obfs, config->obfs_mode, config->obfs_key);
    traffic_limiter_init(&traffic_limiter, config);
    hop_obfs = xgw_obfs_enabled(&obfs) && obfs_hop_mode(config);
    if (!xgw_route_manager_init(&routes, &config->lines, &config->path, config->role, config->hop_name)) {
        set_error(error, error_len, "cannot resolve current hop from path");
        return 0;
    }
    active_line = xgw_route_active(&routes);
    active_hops = runtime_hops(active_line);
    if (active_hops == NULL || active_hops->current == NULL) {
        set_error(error, error_len, "cannot resolve active line");
        return 0;
    }
    if (!runtime_endpoint_addr(active_hops, active_hops->current, host, sizeof(host), &port)) {
        set_error(error, error_len, "invalid current hop address");
        return 0;
    }
    control_peer = active_hops->next != NULL ? active_hops->next : active_hops->previous;
    control_peer_host[0] = '\0';
    if (control_peer != NULL) {
        if (!runtime_endpoint_addr(active_hops, control_peer, control_peer_host, sizeof(control_peer_host), &control_peer_port)) {
            set_error(error, error_len, "invalid control peer address");
            return 0;
        }
    }
    dp = (xgw_dataplane_t *) calloc(1, sizeof(*dp));
    batch = (xgw_frame_batch_t *) calloc(1, sizeof(*batch));
    result = (xgw_reassembly_result_t *) calloc(1, sizeof(*result));
    fec_frame = (uint8_t *) calloc(1, XGW_MAX_FRAME_SIZE);
    tun_buf = (uint8_t *) calloc(1, 65535U);
    packet = (xgw_packet_t *) calloc(1, sizeof(*packet));
    bridge_buf = (uint8_t *) calloc(1, 65535U);
    bridge_targets = (xgw_bridge_target_entry_t *) calloc(XGW_BRIDGE_TARGET_MAP_SIZE, sizeof(*bridge_targets));
    stream_sched = (xgw_stream_scheduler_t *) calloc(1, sizeof(*stream_sched));
    if (dp == NULL || batch == NULL || result == NULL || fec_frame == NULL || tun_buf == NULL || packet == NULL || bridge_buf == NULL || bridge_targets == NULL || stream_sched == NULL) {
        set_error(error, error_len, "runtime allocation failed");
        free(dp);
        free(batch);
        free(result);
        free(fec_frame);
        free(tun_buf);
        free(packet);
        free(bridge_buf);
        free(bridge_targets);
        free(stream_sched);
        return 0;
    }
    xgw_dataplane_init(dp, config);
    g_xgw_log_level = config->tuning.log_level;
    xgw_bridge_set_log_level(config->tuning.log_level);
    xgw_bridge_set_pending_chunks(config->tuning.bridge_egress_pending_chunks);
    session = xgw_session_upsert(&dp->sessions,
                                 1U,
                                 config->allow_policy.group_id,
                                 config->tun_addr,
                                 control_peer_host,
                                 control_peer_port,
                                 time(NULL));
    if (session == NULL) {
        set_error(error, error_len, "cannot allocate outbound session");
        free(dp);
        free(batch);
        free(result);
        free(fec_frame);
        free(tun_buf);
        free(packet);
        return 0;
    }
    if (config->auth_token[0] != '\0') {
        xgw_security_init(&session->security, config->auth_token);
    }
    xgw_cc_init(&session->cc,
                config->congestion_mode,
                config->bbr_profile,
                config->advertised_tx_bps,
                config->profile.pacing_rate_bps,
                config->profile.mtu);
    if (config->tun_name[0] != '\0' && config->tun_addr[0] != '\0') {
        if (!xgw_tun_open(&tun, config->tun_name, config->tun_addr, config->profile.mtu, error, error_len)) {
            goto cleanup;
        }
        tun_opened = 1;
    }
    if (strcmp(config->transport, "afxdp") == 0) {
        if (!xgw_afxdp_open(&xsk, config->device, config->queue_id, port, error, error_len)) {
            goto cleanup;
        }
        net_opened = 1;
    } else {
        if (!xgw_udp_listen(&udp, config->listen_host, port)) {
            printf("runtime.udp.listen.fail role=%s transport=%s phase=net_open listen_host=%s listen_port=%u udp_stage=%s udp_errno=%d udp_error_host=%s udp_error_port=%u\n",
                   xgw_role_name(config->role),
                   config->transport,
                   config->listen_host,
                   (unsigned int) port,
                   xgw_udp_last_error_stage(),
                   xgw_udp_last_error_code(),
                   xgw_udp_last_error_host(),
                   (unsigned int) xgw_udp_last_error_port());
            fflush(stdout);
            snprintf(error,
                     error_len,
                     "cannot open udp socket stage=%s errno=%d host=%s port=%u",
                     xgw_udp_last_error_stage(),
                     xgw_udp_last_error_code(),
                     xgw_udp_last_error_host(),
                     (unsigned int) xgw_udp_last_error_port());
            goto cleanup;
        }
        if (!xgw_udp_set_buffers(&udp, (int) config->tuning.udp_rcvbuf_bytes, (int) config->tuning.udp_sndbuf_bytes)) {
            printf("runtime.warn udp_set_buffers_failed rcv=%u snd=%u\n",
                   config->tuning.udp_rcvbuf_bytes, config->tuning.udp_sndbuf_bytes);
            fflush(stdout);
        }
        if (!config->disable_path_mtu_discovery) {
            int pmtud_ok = xgw_udp_enable_pmtud(&udp);
            printf("runtime.pmtud enable=%d effective_payload=%u ceiling=%u floor=%u\n",
                   pmtud_ok,
                   xgw_dataplane_effective_payload(dp),
                   dp->pmtu.ceiling_payload,
                   dp->pmtu.floor_payload);
            fflush(stdout);
        } else {
            printf("runtime.pmtud disabled (disable_path_mtu_discovery=1)\n");
            fflush(stdout);
        }
        net_opened = 1;
    }

    {
        char current_addr[XGW_MAX_NAME_LEN] = "";
        char prev_addr[XGW_MAX_NAME_LEN] = "";
        char next_addr[XGW_MAX_NAME_LEN] = "";
        xgw_route_endpoint_address(active_hops, active_hops->current, current_addr, sizeof(current_addr));
        xgw_route_endpoint_address(active_hops, active_hops->previous, prev_addr, sizeof(prev_addr));
        xgw_route_endpoint_address(active_hops, active_hops->next, next_addr, sizeof(next_addr));
        printf("runtime.start role=%s transport=%s proxy_mode=%s connect_type=%s acl_mode=%s current=%s prev=%s next=%s\n",
               xgw_role_name(config->role), config->transport, config->proxy_mode, config->connect_type, config->acl_mode,
               current_addr,
               prev_addr,
               next_addr);
    }
    printf("runtime.routes active=%s line_count=%zu control=%s\n",
           runtime_line_id(active_line),
           routes.line_count,
           config->lines.route_control_path);
    log_route_manager_start(&routes);
    printf("runtime.negotiation auth=%s udp=%d congestion=%s bbr_profile=%s rx_bps=%llu tx_bps=%llu windows=%u/%u/%u/%u idle=%u keepalive=%u pmtud_disabled=%d outbound=%s:%u\n",
           config->auth_token[0] != '\0' ? "set" : "unset",
           config->enable_udp,
           xgw_congestion_mode_name(config->congestion_mode),
           xgw_bbr_profile_name(config->bbr_profile),
           (unsigned long long) config->advertised_rx_bps,
           (unsigned long long) config->advertised_tx_bps,
           config->initial_stream_receive_window,
           config->max_stream_receive_window,
           config->initial_connection_receive_window,
           config->max_connection_receive_window,
           config->max_idle_timeout_sec,
           config->keepalive_sec,
           config->disable_path_mtu_discovery,
           config->outbound_host,
           config->outbound_port);
    printf("runtime.traffic_limit enabled=%d baseline_bps=%llu burst_bps=%llu burst_seconds=%u burst_budget_bytes=%llu\n",
           traffic_limiter.enabled,
           (unsigned long long) traffic_limiter.baseline_bps,
           (unsigned long long) traffic_limiter.burst_bps,
           config->tuning.traffic_burst_seconds,
           (unsigned long long) traffic_limiter.capacity_bytes);
    fflush(stdout);

    if (config->outbound_host[0] != '\0' && config->outbound_port > 0U) {
        xgw_outbound_type_t outbound_type = XGW_OUTBOUND_DIRECT;
        if (!xgw_outbound_parse_type(config->outbound_type, &outbound_type)) {
            set_error(error, error_len, "invalid outbound_type");
            goto cleanup;
        }
        if (!xgw_outbound_open(&outbound,
                               outbound_type,
                               "default",
                               config->outbound_host,
                               config->outbound_port,
                               config->outbound_username,
                               config->outbound_password,
                               error,
                               error_len)) {
            goto cleanup;
        }
        snprintf(outbound.current_target_host, sizeof(outbound.current_target_host), "%s", config->outbound_host);
        outbound.current_target_port = config->outbound_port;
    }
    if (is_bridge_connect_type(config)) {
        if (!xgw_bridge_server_start(&bridge, config, error, error_len)) {
            goto cleanup;
        }
    } else if (config->bridge_tcp_listen[0] != '\0') {
        printf("runtime.warn bridge_tcp_listen_ignored connect_type=%s listen=%s\n",
               config->connect_type,
               config->bridge_tcp_listen);
        fflush(stdout);
    }
    if (config->role == XGW_ROLE_EGRESS) {
        if (!xgw_bridge_egress_init(&bridge_egress, error, error_len)) {
            goto cleanup;
        }
        printf("runtime.bridge_egress.marker version=20260612-poll\n");
        fflush(stdout);
        printf("runtime.bridge_egress.init active=%zu previous=%s\n",
               xgw_bridge_egress_active_count(bridge_egress),
               active_hops->previous == NULL ? "" : session->remote_addr.host);
        fflush(stdout);
    }

    while (1) {
        int did_work = 0;
        time_t loop_now = time(NULL);
        if (!xgw_route_poll_control_file(&routes, config->lines.route_control_path, loop_now)) {
            set_error(error, error_len, "route control command failed");
            break;
        }
        xgw_route_tick(&routes, loop_now);
        /* per-stream xport 池回收:对齐 hy2 状态生命周期模型 —— CLOSE 事件主回收
         * (forget_xport_if_close,流真正结束时立即释放),此处 reap 仅作"泄漏兜底"
         * (CLOSE 帧丢失的孤儿 xport)。
         * 关键:超时必须长于"活连接的数据空闲间隙",否则会把还活着、只是暂时没数据的
         * 长连接(frontier/push 等突发流,间隙常 >30s)的 xport 中途回收 → 发送侧 next_tx_seq
         * 重置而接收侧不同步 → 序号空间错位被 replay window 误判 → 卡死数秒(实测根因)。
         * 取 300s,长于活连接数据间隙、对齐 bridge idle_after_first_byte_ms(3~15min)量级,
         * 仍能兜底真正泄漏。节流每 5s 扫一次。 */
        {
            static time_t last_xport_reap = 0;
            if (loop_now != last_xport_reap && (last_xport_reap == 0 || loop_now - last_xport_reap >= 5)) {
                last_xport_reap = loop_now;
                if (dp != NULL) {
                    xgw_stream_xport_reap(&dp->streams, now_us(), 300ULL * 1000000ULL);
                    /* 池占用可观测:正常应远低于容量。持续逼近 XGW_MAX_XPORT_STREAMS 即回收未跟上(泄漏)。
                     * 占用 >1/4 容量才打印,正常静默、异常立现。 */
                    if (dp->streams.count > XGW_MAX_XPORT_STREAMS / 4U) {
                        printf("xport.pool.usage count=%zu cap=%u\n",
                               dp->streams.count, (unsigned int) XGW_MAX_XPORT_STREAMS);
                        fflush(stdout);
                    }
                }
            }
        }
        if (config->tuning.enable_summary_dump && loop_now != last_route_metrics_dump) {
            uint32_t advertised_mbps = (uint32_t) (config->advertised_tx_bps / 1000000ULL);
            if (advertised_mbps == 0U) {
                advertised_mbps = 1000U;
            }
            if (!xgw_route_write_metrics(&routes,
                                         config->lines.metrics_path,
                                         dp == NULL ? 0U : dp->sessions.count,
                                         advertised_mbps)) {
                printf("runtime.warn route_metrics_write_failed path=%s\n", config->lines.metrics_path);
                fflush(stdout);
            }
            last_route_metrics_dump = loop_now;
        }
        if (config->tuning.enable_summary_dump && loop_now != last_cc_dump) {
            size_t sidx;
            printf("runtime.fec groups=%zu completed=%zu data_shards=%u parity_shards=%u\n",
                   dp->fec.group_count,
                   dp->fec.completed_count,
                   dp->fec.data_shards,
                   dp->fec.parity_shards);
            fflush(stdout);
            for (sidx = 0; sidx < dp->sessions.count; ++sidx) {
                if (dp->sessions.sessions[sidx].active) {
                    log_cc_snapshot(&dp->sessions.sessions[sidx]);
                }
            }
            last_cc_dump = loop_now;
            p0_dump();
        }
        active_line = xgw_route_active(&routes);
        active_hops = runtime_hops(active_line);
        runtime_channel_refresh(&runtime_channels, &routes);
        if (active_hops == NULL || active_hops->current == NULL) {
            set_error(error, error_len, "active line missing");
            break;
        }
        stream_sched_gc(stream_sched, monotonic_us());
        if (!config->disable_path_mtu_discovery) {
            uint64_t pmtu_now = monotonic_us();
            if (g_pmtu_too_big_pending) {
                xgw_dataplane_pmtu_note_send(dp, 1, pmtu_now);
                g_pmtu_too_big_pending = 0;
            }
            xgw_dataplane_pmtu_tick(dp, xgw_udp_query_pmtu(&udp), pmtu_now);
        }
        if (config->tuning.enable_summary_dump && config->tuning.summary_path[0] != '\0') {
            if (!write_runtime_summary_json(config, &routes, active_line, dp, stream_sched, &traffic_limiter, loop_now)) {
                printf("runtime.warn summary_write_failed path=%s\n", config->tuning.summary_path);
                fflush(stdout);
            }
        }
        if (config->role == XGW_ROLE_EGRESS) {
            xgw_session_t *established = find_established_peer_session(dp);
            if (established != NULL) {
                session = established;
                if (XGW_RT_VERBOSE()) {
                    printf("runtime.bridge_egress.session_fix host=%s port=%u state=%d\n",
                           session->remote_addr.host,
                           session->remote_addr.port,
                           session->control_state);
                    fflush(stdout);
                }
            } else {
                if (XGW_RT_VERBOSE()) {
                    printf("runtime.bridge_egress.session_fix none\n");
                    fflush(stdout);
                }
            }
        }
        if (bridge != NULL) {
            if (XGW_RT_VERBOSE()) {
                printf("runtime.bridge.poll.begin transport=%s\n", config->bridge_transport);
                fflush(stdout);
            }
            if (!xgw_bridge_server_poll(bridge, 0)) {
                printf("runtime.bridge.poll.end rc=0 transport=%s\n", config->bridge_transport);
                fflush(stdout);
                set_error(error, error_len, "bridge poll failed");
                break;
            }
            if (XGW_RT_VERBOSE()) {
                printf("runtime.bridge.poll.end rc=1 transport=%s\n", config->bridge_transport);
                fflush(stdout);
            }
            {
                int bridge_rc = xgw_bridge_server_dequeue(bridge, bridge_buf, 65535U, &bridge_len, error, error_len);
                if (bridge_rc < 0) {
                    break;
                }
                if (bridge_rc > 0 && bridge_len > 0U && active_hops->next != NULL) {
                    char next_host[64];
                    uint16_t next_port = 0U;
                    xgw_session_t *send_session = NULL;
                    xgw_bridge_target_entry_t *hint = NULL;
                    xgw_flow_hint_t flow_hint;
                    xgw_session_slot_t slot = XGW_SESSION_SLOT_BULK;
                    memset(&flow_hint, 0, sizeof(flow_hint));
                    if (!runtime_endpoint_addr(active_hops, active_hops->next, next_host, sizeof(next_host), &next_port)) {
                        set_error(error, error_len, "invalid next hop address");
                        break;
                    }
                    update_bridge_target_map(bridge_targets, bridge_buf, bridge_len);
                    log_bridge_target_map("runtime.bridge_ingress.map", bridge_targets, bridge_buf, bridge_len);
                    hint = find_bridge_target_from_payload(bridge_targets, bridge_buf, bridge_len);
                    fill_flow_hint_from_target(hint, &flow_hint);
                    if (flow_hint.valid) {
                        slot = xgw_flow_class_to_slot(flow_hint.flow_class);
                    }
                    if (XGW_RT_VERBOSE()) {
                    printf("runtime.bridge_ingress.classify stream=%u target=%s:%u flow_class=%u priority=%u slot=%s frontend=%s front_session_id=%s route=%s line=%s\n",
                           hint == NULL ? 0U : hint->stream_id,
                           next_host,
                           next_port,
                           (unsigned int) flow_hint.flow_class,
                           (unsigned int) flow_hint.priority,
                           runtime_session_slot_name(slot),
                           hint == NULL ? "" : hint->frontend,
                           hint == NULL ? "" : hint->front_session_id,
                           hint == NULL ? "" : hint->route_name,
                           hint == NULL ? "" : hint->line_id);
                    fflush(stdout);
                    }
                    send_session = runtime_channel_session(&runtime_channels,
                                                           runtime_line_id(active_line),
                                                           XGW_CHANNEL_NEXT,
                                                           slot,
                                                           1);
                    if (send_session == NULL) {
                        xgw_runtime_line_channels_t *dbg_line = runtime_line_channels_select(&runtime_channels, runtime_line_id(active_line));
                        xgw_neighbor_channel_t *dbg_channel = dbg_line == NULL ? NULL : xgw_channel_get(&dbg_line->channels, XGW_CHANNEL_NEXT);
                        printf("runtime.bridge_ingress.channel_not_ready target=%s:%u slot=%s session_state=%d legacy_state=%d action=defer_enqueue\n",
                               next_host,
                               next_port,
                               runtime_session_slot_name(slot),
                               send_session == NULL ? -1 : (int) send_session->control_state,
                               session == NULL ? -1 : (int) session->control_state);
                        if (dbg_channel != NULL) {
                            log_channel_state("runtime.bridge_ingress.channel_not_ready.state", runtime_line_id(active_line), dbg_channel);
                        }
                        fflush(stdout);
                        if (!stream_sched_enqueue(stream_sched,
                                                  bridge_buf,
                                                  bridge_len,
                                                  runtime_line_id(active_line),
                                                  XGW_CHANNEL_NEXT,
                                                  hint)) {
                            printf("runtime.warn stream_sched_drop stage=bridge_ingress_defer stream_active=%zu target=%s:%u payload=%zu\n",
                                   stream_sched_active_count(stream_sched),
                                   next_host,
                                   next_port,
                                   bridge_len);
                            fflush(stdout);
                            forget_bridge_target_if_close(bridge_targets, bridge_buf, bridge_len);
                            did_work = 1;
                            continue;
                        }
                        forget_bridge_target_if_close(bridge_targets, bridge_buf, bridge_len);
                        forget_scheduler_if_close(stream_sched, bridge_buf, bridge_len);
                        forget_xport_if_close(&dp->streams, bridge_buf, bridge_len);
                        did_work = 1;
                        continue;
                    }
                    if (send_session == NULL) {
                        set_error(error, error_len, "send session alloc failed");
                        break;
                    }
                    log_session_summary("runtime.bridge_ingress.session", send_session);
                    if (XGW_RT_VERBOSE()) {
                    printf("runtime.bridge_ingress.send line=%s target=%s:%u state=%d peer=%s:%u payload=%zu\n",
                           runtime_line_id(active_line),
                           next_host, next_port,
                           send_session->control_state,
                           send_session->remote_addr.host,
                           send_session->remote_addr.port,
                           bridge_len);
                    fflush(stdout);
                    }
                    xgw_dataplane_set_flow_hint(dp, hint == NULL ? NULL : &flow_hint);
                    if (!xgw_dataplane_build_outbound(dp, send_session, bridge_buf, bridge_len, batch, fec_frame, XGW_MAX_FRAME_SIZE, &fec_len)) {
                        set_error(error, error_len, xgw_dataplane_last_error());
                        break;
                    }
                    if (XGW_RT_VERBOSE()) {
                        size_t bi;
                        for (bi = 0U; bi < batch->frame_count; ++bi) {
                            xgw_header_t dbg_header;
                            const uint8_t *dbg_payload = NULL;
                            size_t dbg_payload_len = 0U;
                            if (xgw_header_decode(batch->frames[bi],
                                                  batch->frame_lengths[bi],
                                                  &dbg_header,
                                                  &dbg_payload,
                                                  &dbg_payload_len)) {
                                printf("runtime.bridge_ingress.frame stream=%u idx=%zu type=%u seq=%llu frame_len=%zu payload_len=%zu\n",
                                       hint == NULL ? 0U : hint->stream_id,
                                       bi,
                                       (unsigned int) dbg_header.type,
                                       (unsigned long long) dbg_header.sequence,
                                       batch->frame_lengths[bi],
                                       dbg_payload_len);
                            } else {
                                printf("runtime.bridge_ingress.frame_decode_fail idx=%zu frame_len=%zu\n",
                                       bi,
                                       batch->frame_lengths[bi]);
                            }
                            fflush(stdout);
                        }
                    }
                    if (!stream_sched_enqueue(stream_sched,
                                              bridge_buf,
                                              bridge_len,
                                              runtime_line_id(active_line),
                                              XGW_CHANNEL_NEXT,
                                              hint)) {
                        printf("runtime.warn stream_sched_drop stage=bridge_ingress stream_active=%zu target=%s:%u payload=%zu\n",
                               stream_sched_active_count(stream_sched),
                               next_host,
                               next_port,
                               bridge_len);
                        fflush(stdout);
                        forget_bridge_target_if_close(bridge_targets, bridge_buf, bridge_len);
                        did_work = 1;
                        continue;
                    }
                    forget_bridge_target_if_close(bridge_targets, bridge_buf, bridge_len);
                    forget_scheduler_if_close(stream_sched, bridge_buf, bridge_len);
                    did_work = 1;
                }
            }
        }
        if (bridge_egress != NULL && active_hops->previous != NULL) {
            if (config->auth_token[0] != '\0' && session->control_state != XGW_CTRL_ESTABLISHED && config->role == XGW_ROLE_EGRESS) {
                xgw_session_t *established = find_established_peer_session(dp);
                if (established != NULL) {
                    session = established;
                    printf("runtime.bridge_egress.session_fix host=%s port=%u state=%d\n",
                           session->remote_addr.host,
                           session->remote_addr.port,
                           session->control_state);
                    fflush(stdout);
                }
            }
            /* egress 控制会话未建立前不回程（旧逻辑用 goto 跳过，这里用条件包裹整段 burst）。 */
            if (!(config->auth_token[0] != '\0' && session->control_state != XGW_CTRL_ESTABLISHED)) {
            if (XGW_RT_VERBOSE()) {
                printf("runtime.bridge_egress.loop active=%zu session_state=%d prev=%s\n",
                   xgw_bridge_egress_active_count(bridge_egress),
                   session->control_state,
                   session->remote_addr.host);
                fflush(stdout);
            }
            {
                /* burst 回程出队：单次全表 drain + 预算内连续吐多帧（消除每 tick 1 chunk 节流）。
                 * poll_all 不再单独调用，drain 已并入 burst 的首遍扫描。 */
                runtime_bridge_egress_ctx_t ectx;
                int burst_rc;
                memset(&ectx, 0, sizeof(ectx));
                ectx.config = config;
                ectx.dp = dp;
                ectx.session = session;
                ectx.runtime_channels = &runtime_channels;
                ectx.active_line = active_line;
                ectx.active_hops = active_hops;
                ectx.bridge_egress = bridge_egress;
                ectx.bridge_targets = bridge_targets;
                ectx.stream_sched = stream_sched;
                ectx.batch = batch;
                ectx.fec_frame = fec_frame;
                ectx.error = error;
                ectx.error_len = error_len;
                burst_rc = xgw_bridge_egress_dequeue_burst_ex(
                    bridge_egress,
                    config->tuning.bridge_egress_dequeue_max_chunks_per_tick,
                    config->tuning.bridge_egress_dequeue_max_bytes_per_tick,
                    (uint64_t) config->tuning.bridge_egress_dequeue_max_us_per_tick,
                    runtime_bridge_egress_emit,
                    &ectx,
                    error,
                    error_len);
                fec_len = ectx.fec_len;
                if (ectx.did_work) {
                    did_work = 1;
                }
                if (burst_rc < 0) {
                    break;
                }
                if (burst_rc > 0 && config->tuning.enable_summary_dump &&
                    loop_now != last_egress_burst_dump) {
                    printf("runtime.bridge_egress.burst chunks=%d active=%zu\n",
                           burst_rc,
                           xgw_bridge_egress_active_count(bridge_egress));
                    fflush(stdout);
                    last_egress_burst_dump = loop_now;
                }
            }
            }
        }
        if (strcmp(config->transport, "udp") == 0 &&
            !runtime_channel_manager_tick(&udp, dp, config, &dp->negotiation, &obfs, hop_obfs, &runtime_channels, error, error_len)) {
            break;
        }
        if (strcmp(config->transport, "udp") == 0) {
            const xgw_line_runtime_t *candidate = xgw_route_candidate(&routes);
            if (candidate != NULL && routes.requested_active_line[0] != '\0' &&
                strcmp(routes.requested_active_line, runtime_line_id(candidate)) == 0) {
                xgw_channel_direction_t candidate_dir = candidate->hops.next != NULL ? XGW_CHANNEL_NEXT : XGW_CHANNEL_PREVIOUS;
                if (runtime_line_channel_ready(&runtime_channels, runtime_line_id(candidate), candidate_dir)) {
                    if (!xgw_route_commit_requested(&routes, loop_now)) {
                        set_error(error, error_len, "commit requested line failed");
                        break;
                    }
                    active_line = xgw_route_active(&routes);
                    active_hops = runtime_hops(active_line);
                    runtime_channel_refresh(&runtime_channels, &routes);
                }
            }
        }
        if (strcmp(config->transport, "udp") == 0) {
            if (!flush_pending_feedbacks_udp(&udp, dp, &obfs, hop_obfs)) {
                set_error(error, error_len, "send ack feedback failed");
                break;
            }
        }
        if (strcmp(config->transport, "udp") == 0) {
            stream_sched_tick(stream_sched, monotonic_us());
            xgw_stream_sched_entry_t *sched_entry = stream_sched_pick(stream_sched, &runtime_channels);
            if (sched_entry != NULL) {
                xgw_session_t *send_session = runtime_channel_session_for_sched(&runtime_channels,
                                                                                sched_entry->line_id,
                                                                                sched_entry->send_direction,
                                                                                sched_entry->session_slot,
                                                                                1);
                if (send_session == NULL) {
                    static uint64_t last_cnr_log_us = 0U;
                    uint64_t cnr_now_us = monotonic_us();
                    if (last_cnr_log_us == 0U || cnr_now_us - last_cnr_log_us >= 500000ULL) {
                        last_cnr_log_us = cnr_now_us;
                        printf("runtime.stream_sched.channel_not_ready stream=%u line=%s dir=%d slot=%s\n",
                               sched_entry->stream_id,
                               sched_entry->line_id,
                               (int) sched_entry->send_direction,
                               runtime_session_slot_name(sched_entry->session_slot));
                        fflush(stdout);
                    }
                    continue;
                }
                fill_flow_hint_runtime_metrics(sched_entry, &sched_entry->hint);
                xgw_dataplane_set_flow_hint(dp, sched_entry->hint.valid ? &sched_entry->hint : NULL);
                if (!xgw_dataplane_build_outbound(dp,
                                                  send_session,
                                                  sched_entry->payload,
                                                  sched_entry->payload_len,
                                                  batch,
                                                  fec_frame,
                                                  XGW_MAX_FRAME_SIZE,
                                                  &fec_len)) {
                    set_error(error, error_len, xgw_dataplane_last_error());
                    break;
                }
                if (!send_frame_series_udp(&udp,
                                           &traffic_limiter,
                                           send_session,
                                           &dp->streams,
                                           config,
                                           &obfs,
                                           hop_obfs,
                                           send_session->remote_addr.host,
                                           send_session->remote_addr.port,
                                           batch,
                                           sched_entry->hint.valid ? &sched_entry->hint : NULL)) {
                    set_error(error, error_len, "scheduled udp send failed");
                    break;
                }
                printf("runtime.stream_sched.sent stream=%u frontend=%s front_session_id=%s route=%s line=%s dir=%d slot=%s priority=%u bytes=%zu inflight=%llu send_credit=%llu ack_debt=%u parity_budget=%u sent_total=%llu returned_total=%llu\n",
                       sched_entry->stream_id,
                       sched_entry->frontend,
                       sched_entry->front_session_id,
                       sched_entry->route_name,
                       sched_entry->line_id,
                       (int) sched_entry->send_direction,
                       runtime_session_slot_name(sched_entry->session_slot),
                       sched_entry->hint.priority,
                       sched_entry->payload_len,
                       (unsigned long long) sched_entry->inflight_bytes,
                       (unsigned long long) sched_entry->send_credit_bytes,
                       sched_entry->ack_credit_frames,
                       sched_entry->parity_budget,
                       (unsigned long long) sched_entry->sent_bytes_total,
                       (unsigned long long) sched_entry->delivered_return_bytes);
                fflush(stdout);
                /* P0: 首次出队记排队时长(enqueued→实际发出)；按方向切分。 */
                if (sched_entry->last_sent_us == 0U && sched_entry->enqueued_us > 0U) {
                    uint64_t nowq = monotonic_us();
                    if (nowq > sched_entry->enqueued_us) {
                        p0_note_queue_wait(nowq - sched_entry->enqueued_us,
                                           sched_entry->send_direction == XGW_CHANNEL_PREVIOUS);
                    }
                }
                stream_sched_mark_sent(sched_entry);
                did_work = 1;
            }
        }
        if (tun_opened && active_hops->next != NULL && (config->auth_token[0] == '\0' || session->control_state == XGW_CTRL_ESTABLISHED)) {
            if (xgw_tun_read(&tun, tun_buf, 65535U, &tun_len) && tun_len > 0U) {
                if (strcmp(config->transport, "afxdp") == 0) {
                    size_t i;
                    for (i = 0; i < batch->frame_count; ++i) {
                        if (!xgw_afxdp_send(&xsk, batch->frames[i], batch->frame_lengths[i])) {
                            set_error(error, error_len, "af_xdp send failed");
                            break;
                        }
                        maybe_pace_session(&traffic_limiter, session, batch->frame_lengths[i]);
                    }
                } else {
                    const xgw_pool_node_t *pool_node = select_pool_node(dp);
                    if (pool_node != NULL && strcmp(config->proxy_mode, "fixed-path") != 0) {
                        xgw_bridge_target_entry_t *hint = find_bridge_target_by_host_port(bridge_targets,
                                                                                           outbound.current_target_host,
                                                                                           outbound.current_target_port);
                        xgw_flow_hint_t flow_hint;
                        fill_flow_hint_from_target(hint, &flow_hint);
                        xgw_dataplane_set_flow_hint(dp, hint == NULL ? NULL : &flow_hint);
                        if (!xgw_dataplane_build_outbound(dp, session, tun_buf, tun_len, batch, fec_frame, XGW_MAX_FRAME_SIZE, &fec_len)) {
                            set_error(error, error_len, xgw_dataplane_last_error());
                            break;
                        }
                        if (!send_frame_series_outbound(&outbound, &traffic_limiter, session, &obfs, hop_obfs, batch, hint == NULL ? NULL : &flow_hint)) {
                            set_error(error, error_len, "outbound send failed");
                            break;
                        }
                    } else {
                        char next_host[64];
                        uint16_t next_port = 0U;
                        xgw_session_t *send_session = NULL;
                        if (!runtime_endpoint_addr(active_hops, active_hops->next, next_host, sizeof(next_host), &next_port)) {
                            set_error(error, error_len, "invalid next hop address");
                            break;
                        }
                        send_session = runtime_channel_session(&runtime_channels,
                                                               runtime_line_id(active_line),
                                                               XGW_CHANNEL_NEXT,
                                                               /* tun 直发无业务分类，固定走 BULK slot；
                                                                * 不读 dp->current_flow_hint.flow_class（上一帧残留，
                                                                * 与 P0 收包中性化同源的跨流污染）。 */
                                                               XGW_SESSION_SLOT_BULK,
                                                               1);
                        if (send_session == NULL) {
                            printf("runtime.tun.channel_not_ready target=%s:%u\n", next_host, next_port);
                            fflush(stdout);
                            did_work = 1;
                            break;
                        }
                        if (send_session == NULL) {
                            set_error(error, error_len, "send session alloc failed");
                            break;
                        }
                        xgw_dataplane_set_flow_hint(dp, NULL);
                        if (!xgw_dataplane_build_outbound(dp, send_session, tun_buf, tun_len, batch, fec_frame, XGW_MAX_FRAME_SIZE, &fec_len)) {
                            set_error(error, error_len, xgw_dataplane_last_error());
                            break;
                        }
                        if (!send_frame_series_udp(&udp, &traffic_limiter, send_session, &dp->streams, config, &obfs, hop_obfs, next_host, next_port, batch, NULL)) {
                            set_error(error, error_len, "udp send failed");
                            break;
                        }
                    }
                }
                did_work = 1;
            }
        }

        memset(packet, 0, sizeof(*packet));
        if (strcmp(config->transport, "afxdp") == 0) {
            size_t rx_len = 0U;
            if (xgw_afxdp_recv(&xsk, packet->data, sizeof(packet->data), &rx_len)) {
                packet->data_len = rx_len;
                did_work = 1;
            }
        } else {
            int recv_rc = xgw_udp_recv(&udp, packet, 5);
            if (recv_rc < 0) {
                set_error(error, error_len, "udp receive failed");
                break;
            }
            if (recv_rc > 0) {
                if (hop_obfs) {
                    uint8_t plain[65535];
                    size_t plain_len = xgw_obfs_decode(&obfs, packet->data, packet->data_len, plain, sizeof(plain));
                    if (plain_len > 0U) {
                        memcpy(packet->data, plain, plain_len);
                        packet->data_len = plain_len;
                    }
                }
                did_work = 1;
            }
        }

        if (packet->data_len > 0U) {
            char summary[256];
            const xgw_line_runtime_t *rx_line = NULL;
            const xgw_node_hops_t *rx_hops = NULL;
            xgw_session_t *packet_session = NULL;
            if (XGW_RT_VERBOSE()) printf("runtime.recv len=%zu remote=%s:%u\n",
                   packet->data_len,
                   packet->remote_host[0] == '\0' ? "-" : packet->remote_host,
                   packet->remote_port);
            fflush(stdout);
            memset(result, 0, sizeof(*result));
            xgw_dataplane_process_frame(dp, packet, result, summary, sizeof(summary));
            if (XGW_RT_VERBOSE()) printf("runtime.proc %s\n", summary);
            fflush(stdout);
            packet_session = xgw_session_find_by_peer(&dp->sessions,
                                                      result->session_id != 0U ? result->session_id : 1U,
                                                      packet->remote_host,
                                                      packet->remote_port);
            if (packet_session != NULL &&
                strstr(summary, "ok=control") != NULL &&
                strstr(summary, "established=1") != NULL) {
                if (XGW_RT_VERBOSE()) {
                printf("runtime.channel.promote peer=%s:%u session_state=%d\n",
                       packet->remote_host,
                       packet->remote_port,
                       packet_session->control_state);
                fflush(stdout);
                }
            }
            if (packet_session != NULL && packet_session->control_state == XGW_CTRL_ESTABLISHED) {
                runtime_channels_promote_peer(&runtime_channels,
                                              packet->remote_host,
                                              packet->remote_port,
                                              packet_session);
            }
            rx_line = runtime_line_for_peer(&routes, active_line, packet->remote_host, packet->remote_port);
            rx_hops = runtime_hops(rx_line);
            if (XGW_RT_VERBOSE()) printf("runtime.recv.route line=%s state=%s remote=%s:%u\n",
                   runtime_line_id(rx_line),
                   rx_line == NULL ? "" : xgw_line_state_name(rx_line->state),
                   packet->remote_host,
                   packet->remote_port);
            fflush(stdout);
            if (config->role == XGW_ROLE_EGRESS) {
                if (packet_session != NULL && packet_session->control_state == XGW_CTRL_ESTABLISHED) {
                    session = packet_session;
                    printf("runtime.bridge_egress.peer_switch host=%s port=%u state=%d\n",
                           session->remote_addr.host,
                           session->remote_addr.port,
                           session->control_state);
                    fflush(stdout);
                }
            }

            if (result->control_frame_len > 0U && strcmp(config->transport, "udp") == 0) {
                xgw_session_t *control_session = packet_session != NULL ? packet_session : session;
                if (!send_control_frame_udp(&udp,
                                            control_session,
                                            &obfs,
                                            hop_obfs,
                                            packet->remote_host,
                                            packet->remote_port,
                                            result->control_frame,
                                            result->control_frame_len)) {
                    set_error(error, error_len, "send control response failed");
                    break;
                }
            }

            if (result->packet_len > 0U) {
                if (XGW_RT_VERBOSE()) printf("runtime.packet.dispatch role=%s len=%zu bridge=%d bridge_egress=%d\n",
                       xgw_role_name(config->role),
                       result->packet_len,
                       bridge != NULL,
                       bridge_egress != NULL);
                fflush(stdout);
                if (config->role == XGW_ROLE_EGRESS) {
                    log_bridge_payload_summary("runtime.egress.packet", result->packet, result->packet_len);
                    update_bridge_target_map(bridge_targets, result->packet, result->packet_len);
                    log_bridge_target_map("runtime.egress.map", bridge_targets, result->packet, result->packet_len);
                    {
                        uint32_t egress_stream = 0U;
                        uint8_t egress_kind = 0U;
                        uint16_t egress_len = 0U;
                        if (parse_bridge_payload_meta(result->packet,
                                                      result->packet_len,
                                                      &egress_stream,
                                                      &egress_kind,
                                                      &egress_len)) {
                            printf("runtime.egress.packet.meta stream=%u kind=%u data_len=%u remote=%s:%u\n",
                                   egress_stream,
                                   (unsigned int) egress_kind,
                                   (unsigned int) egress_len,
                                   packet->remote_host,
                                   packet->remote_port);
                        } else {
                            printf("runtime.egress.packet.meta invalid len=%zu remote=%s:%u\n",
                                   result->packet_len,
                                   packet->remote_host,
                                   packet->remote_port);
                        }
                        fflush(stdout);
                    }
                }
                if (bridge != NULL) {
                    uint32_t delivered_stream = 0U;
                    uint16_t delivered_len = 0U;
                    uint8_t delivered_kind = 0U;
                    parse_bridge_payload_meta(result->packet, result->packet_len, &delivered_stream, &delivered_kind, &delivered_len);
                    if (config->role == XGW_ROLE_INGRESS && delivered_stream != 0U) {
                        xgw_bridge_target_entry_t *ret_entry = find_bridge_target_entry(bridge_targets, delivered_stream);
                        printf("runtime.ingress.return.packet stream=%u kind=%u data_len=%u remote=%s:%u payload=%zu\n",
                               delivered_stream,
                               (unsigned int) delivered_kind,
                               (unsigned int) delivered_len,
                               packet->remote_host,
                               packet->remote_port,
                               result->packet_len);
                        if (ret_entry != NULL) {
                            printf("runtime.ingress.return.meta stream=%u frontend=%s front_session_id=%s route=%s line=%s target=%s:%u\n",
                                   delivered_stream,
                                   ret_entry->frontend,
                                   ret_entry->front_session_id,
                                   ret_entry->route_name,
                                   ret_entry->line_id,
                                   ret_entry->target_host,
                                   ret_entry->target_port);
                        }
                        fflush(stdout);
                    }
                    int bridge_payload = xgw_bridge_server_handle_payload(bridge, result->packet, result->packet_len, error, error_len);
                    if (bridge_payload < 0) {
                        break;
                    }
                    if (bridge_payload > 0) {
                        if (delivered_stream != 0U && (delivered_kind == 2U || delivered_kind == 5U)) {
                            stream_sched_mark_return(stream_sched, delivered_stream, delivered_len);
                        }
                        forget_bridge_target_if_close(bridge_targets, result->packet, result->packet_len);
                        forget_scheduler_if_close(stream_sched, result->packet, result->packet_len);
                        forget_xport_if_close(&dp->streams, result->packet, result->packet_len);
                        did_work = 1;
                        goto packet_done;
                    }
                }
                if (bridge_egress != NULL) {
                    if (config->role == XGW_ROLE_EGRESS) {
                        printf("runtime.egress.bridge_gate len=%zu active=%zu peer=%s:%u session_state=%d\n",
                               result->packet_len,
                               xgw_bridge_egress_active_count(bridge_egress),
                               packet->remote_host,
                               packet->remote_port,
                               packet_session == NULL ? -1 : (int) packet_session->control_state);
                        fflush(stdout);
                    }
                    if (config->role == XGW_ROLE_EGRESS) {
                        printf("runtime.egress.bridge_handle.begin len=%zu remote=%s:%u\n",
                               result->packet_len,
                               packet->remote_host,
                               packet->remote_port);
                        fflush(stdout);
                    }
                    int bridge_payload = xgw_bridge_egress_handle_payload(bridge_egress,
                                                                          packet->remote_host,
                                                                          packet->remote_port,
                                                                          result->packet,
                                                                          result->packet_len,
                                                                          error,
                                                                          error_len);
                    if (config->role == XGW_ROLE_EGRESS) {
                        printf("runtime.egress.bridge_handle.end rc=%d len=%zu remote=%s:%u\n",
                               bridge_payload,
                               result->packet_len,
                               packet->remote_host,
                               packet->remote_port);
                        fflush(stdout);
                    }
                    if (bridge_payload < 0) {
                        printf("runtime.warn bridge_egress_handle failed packet_len=%zu error=%s\n",
                               result->packet_len,
                               error == NULL ? "" : error);
                        fflush(stdout);
                        did_work = 1;
                        goto packet_done;
                    }
                    if (bridge_payload > 0) {
                        did_work = 1;
                        goto packet_done;
                    }
                }
                if (config->role == XGW_ROLE_INGRESS && bridge != NULL) {
                    int bridge_payload = xgw_bridge_server_handle_payload(bridge, result->packet, result->packet_len, error, error_len);
                    printf("runtime.ingress.bridge_handle rc=%d len=%zu remote=%s:%u\n",
                           bridge_payload,
                           result->packet_len,
                           packet->remote_host,
                           packet->remote_port);
                    fflush(stdout);
                    if (bridge_payload < 0) {
                        printf("runtime.warn bridge_ingress_handle failed packet_len=%zu error=%s\n",
                               result->packet_len,
                               error == NULL ? "" : error);
                        fflush(stdout);
                        did_work = 1;
                        goto packet_done;
                    }
                    if (bridge_payload > 0) {
                        did_work = 1;
                        goto packet_done;
                    }
                    printf("runtime.ingress.local_drop len=%zu remote=%s:%u\n",
                           result->packet_len,
                           packet->remote_host,
                           packet->remote_port);
                    fflush(stdout);
                    did_work = 1;
                    goto packet_done;
                }
                {
                    /* UDP 转发断点定向诊断（节流）：打印 forward 入口三条件值，定位 UDP 帧为何跳过转发。 */
                    static uint64_t last_fwd_diag_us = 0U;
                    uint8_t diag_kind = 0U;
                    parse_bridge_payload_meta(result->packet, result->packet_len, NULL, &diag_kind, NULL);
                    if (is_forwarder(config->role) &&
                        (diag_kind == 3U || diag_kind == 5U || diag_kind == 6U)) {
                        uint64_t dnow = monotonic_us();
                        if (last_fwd_diag_us == 0U || dnow - last_fwd_diag_us >= 300000ULL) {
                            last_fwd_diag_us = dnow;
                            printf("runtime.fwd.diag role=%s kind=%u packet_len=%zu rx_line=%s rx_hops=%d next=%d remote=%s:%u\n",
                                   xgw_role_name(config->role),
                                   (unsigned int) diag_kind,
                                   result->packet_len,
                                   runtime_line_id(rx_line),
                                   rx_hops != NULL,
                                   (rx_hops != NULL && rx_hops->next != NULL),
                                   packet->remote_host,
                                   packet->remote_port);
                            fflush(stdout);
                        }
                    }
                }
                if (is_forwarder(config->role) && rx_hops != NULL && rx_hops->next != NULL) {
                    const xgw_endpoint_t *forward_target = select_forward_target(rx_hops, config, packet, packet_session);
                    xgw_session_t *send_session = NULL;
                    char forward_host[64];
                    uint16_t forward_port = 0U;
                    xgw_channel_direction_t forward_dir = XGW_CHANNEL_NONE;
                    if (XGW_RT_VERBOSE()) {
                    printf("runtime.forward.enter role=%s line=%s packet_len=%zu\n",
                           xgw_role_name(config->role),
                           runtime_line_id(rx_line),
                           result->packet_len);
                    fflush(stdout);
                    if (packet_session != NULL) {
                        printf("runtime.forward.packet_session peer=%s:%u state=%d\n",
                               packet_session->remote_addr.host,
                               packet_session->remote_addr.port,
                               packet_session->control_state);
                    } else {
                        printf("runtime.forward.packet_session none remote=%s:%u\n",
                               packet->remote_host,
                               packet->remote_port);
                    }
                    fflush(stdout);
                    }
                    log_bridge_payload_summary("runtime.forward.bridge", result->packet, result->packet_len);
                    update_bridge_target_map(bridge_targets, result->packet, result->packet_len);
                    log_bridge_target_map("runtime.forward.map", bridge_targets, result->packet, result->packet_len);
                    note_relay_target_stage(bridge_targets,
                                            config,
                                            rx_hops,
                                            packet,
                                            packet_session,
                                            result->packet,
                                            result->packet_len);
                    if (forward_target == NULL ||
                        !runtime_endpoint_addr(rx_hops, forward_target, forward_host, sizeof(forward_host), &forward_port)) {
                        set_error(error, error_len, "invalid forward target address");
                        break;
                    }
                    if (forward_target == rx_hops->next) {
                        forward_dir = XGW_CHANNEL_NEXT;
                    } else if (forward_target == rx_hops->previous) {
                        forward_dir = XGW_CHANNEL_PREVIOUS;
                    }
                    {
                        xgw_runtime_line_channels_t *line_channels = runtime_line_channels_select(&runtime_channels, runtime_line_id(rx_line));
                        xgw_neighbor_channel_t *channel = line_channels == NULL ? NULL : xgw_channel_get(&line_channels->channels, forward_dir);
                        if (XGW_RT_VERBOSE()) {
                        printf("runtime.forward.channel.select line=%s dir=%d target=%s:%u chan_peer=%s:%u pending_ptr=%p pending_state=%d active_ptr=%p active_state=%d\n",
                               runtime_line_id(rx_line),
                               (int) forward_dir,
                               forward_host,
                               forward_port,
                               channel == NULL ? "" : channel->peer_host,
                               channel == NULL ? 0U : channel->peer_port,
                               channel == NULL ? NULL : (void *) channel->pending_session,
                               channel == NULL || channel->pending_session == NULL ? -1 : (int) channel->pending_session->control_state,
                               channel == NULL ? NULL : (void *) channel->active_session,
                               channel == NULL || channel->active_session == NULL ? -1 : (int) channel->active_session->control_state);
                        fflush(stdout);
                        }
                    }
                    if (send_session == NULL) {
                        xgw_bridge_target_entry_t *hint = find_bridge_target_from_payload(bridge_targets, result->packet, result->packet_len);
                        xgw_session_slot_t slot = XGW_SESSION_SLOT_BULK;
                        if (hint != NULL) {
                            slot = xgw_flow_class_to_slot(hint->flow_class);
                        }
                        if (XGW_RT_VERBOSE()) {
                        printf("runtime.forward.classify stream=%u target=%s:%u flow_class=%u priority=%u slot=%s frontend=%s front_session_id=%s route=%s line=%s\n",
                               hint == NULL ? 0U : hint->stream_id,
                               forward_host,
                               forward_port,
                               hint == NULL ? 0U : (unsigned int) hint->flow_class,
                               hint == NULL ? 0U : (unsigned int) hint->priority,
                               runtime_session_slot_name(slot),
                               hint == NULL ? "" : hint->frontend,
                               hint == NULL ? "" : hint->front_session_id,
                               hint == NULL ? "" : hint->route_name,
                               hint == NULL ? "" : hint->line_id);
                        fflush(stdout);
                        }
                        send_session = runtime_channel_session(&runtime_channels,
                                                               runtime_line_id(rx_line),
                                                               forward_dir,
                                                               slot,
                                                               1);
                    }
                    if (send_session == NULL) {
                        if (XGW_RT_VERBOSE()) {
                        {
                            xgw_runtime_line_channels_t *line_channels = runtime_line_channels_select(&runtime_channels, runtime_line_id(rx_line));
                            xgw_neighbor_channel_t *channel = line_channels == NULL ? NULL : xgw_channel_get(&line_channels->channels, forward_dir);
                            printf("channel.active.miss line=%s dir=%d target=%s:%u pending_ptr=%p pending_state=%d active_ptr=%p active_state=%d\n",
                                   runtime_line_id(rx_line),
                                   (int) forward_dir,
                                   forward_host,
                                   forward_port,
                                   channel == NULL ? NULL : (void *) channel->pending_session,
                                   channel == NULL || channel->pending_session == NULL ? -1 : (int) channel->pending_session->control_state,
                                   channel == NULL ? NULL : (void *) channel->active_session,
                                   channel == NULL || channel->active_session == NULL ? -1 : (int) channel->active_session->control_state);
                        }
                        printf("runtime.forward.channel_not_ready dir=%d target=%s:%u\n",
                               (int) forward_dir,
                               forward_host,
                               forward_port);
                        printf("runtime.forward.exit reason=channel_not_ready line=%s dir=%d target=%s:%u\n",
                               runtime_line_id(rx_line),
                               (int) forward_dir,
                               forward_host,
                               forward_port);
                        fflush(stdout);
                        }
                        did_work = 1;
                        goto packet_done;
                    }
                    if (send_session == NULL) {
                        set_error(error, error_len, "send session alloc failed");
                        break;
                    }
                    log_session_summary("runtime.forward.send_session", send_session);
                    {
                        /* 污染修复：在 build_outbound 之前查到当前转发流的 hint 并 set，
                         * 否则发送侧 FEC/parity 会吃到上一帧残留的 current_flow_hint。 */
                        xgw_bridge_target_entry_t *fwd_hint = find_bridge_target_from_payload(bridge_targets, result->packet, result->packet_len);
                        xgw_flow_hint_t fwd_flow_hint;
                        fill_flow_hint_from_target(fwd_hint, &fwd_flow_hint);
                        xgw_dataplane_set_flow_hint(dp, fwd_hint == NULL ? NULL : &fwd_flow_hint);
                    }
                    {
                        xgw_runtime_line_channels_t *line_channels = runtime_line_channels_select(&runtime_channels, runtime_line_id(rx_line));
                        xgw_neighbor_channel_t *channel = line_channels == NULL ? NULL : xgw_channel_get(&line_channels->channels, forward_dir);
                        if (XGW_RT_VERBOSE()) {
                        printf("channel.active.use line=%s dir=%d target=%s:%u pending_ptr=%p pending_state=%d active_ptr=%p active_state=%d\n",
                               runtime_line_id(rx_line),
                               (int) forward_dir,
                               forward_host,
                               forward_port,
                               channel == NULL ? NULL : (void *) channel->pending_session,
                               channel == NULL || channel->pending_session == NULL ? -1 : (int) channel->pending_session->control_state,
                               channel == NULL ? NULL : (void *) channel->active_session,
                               channel == NULL || channel->active_session == NULL ? -1 : (int) channel->active_session->control_state);
                        fflush(stdout);
                        }
                    }
                    if (XGW_RT_VERBOSE()) {
                    printf("runtime.forward.send target=%s:%u state=%d peer=%s:%u payload=%zu\n",
                           forward_host, forward_port,
                           send_session->control_state,
                           send_session->remote_addr.host,
                           send_session->remote_addr.port,
                           result->packet_len);
                    fflush(stdout);
                    }
                    if (!xgw_dataplane_build_outbound(dp, send_session, result->packet, result->packet_len, batch, fec_frame, XGW_MAX_FRAME_SIZE, &fec_len)) {
                        printf("runtime.forward.exit reason=build_outbound_fail line=%s dir=%d target=%s:%u err=%s\n",
                               runtime_line_id(rx_line),
                               (int) forward_dir,
                               forward_host,
                               forward_port,
                               xgw_dataplane_last_error());
                        fflush(stdout);
                        set_error(error, error_len, xgw_dataplane_last_error());
                        break;
                    }
                    if (strcmp(config->transport, "udp") == 0) {
                        xgw_bridge_target_entry_t *hint = find_bridge_target_from_payload(bridge_targets, result->packet, result->packet_len);
                        xgw_flow_hint_t flow_hint;
                        fill_flow_hint_from_target(hint, &flow_hint);
                        (void) flow_hint; /* hint 已在 build_outbound 前 set；此处仅取 hint 供 enqueue。 */
                        if (!stream_sched_enqueue(stream_sched,
                                                  result->packet,
                                                  result->packet_len,
                                                  runtime_line_id(rx_line),
                                                  forward_dir,
                                                  hint)) {
                            printf("runtime.warn stream_sched_drop stage=forward stream_active=%zu target=%s:%u payload=%zu\n",
                                   stream_sched_active_count(stream_sched),
                                   forward_host,
                                   forward_port,
                                   result->packet_len);
                            fflush(stdout);
                            printf("runtime.forward.exit reason=stream_sched_drop line=%s dir=%d target=%s:%u\n",
                                   runtime_line_id(rx_line),
                                   (int) forward_dir,
                                   forward_host,
                                   forward_port);
                            fflush(stdout);
                            forget_bridge_target_if_close(bridge_targets, result->packet, result->packet_len);
                            did_work = 1;
                            goto packet_done;
                        }
                        if (XGW_RT_VERBOSE()) {
                        printf("runtime.forward.queued role=%s target=%s:%u frames=%zu\n",
                               xgw_role_name(config->role),
                               forward_host,
                               forward_port,
                               batch->frame_count);
                        fflush(stdout);
                        printf("runtime.forward.exit reason=queued line=%s dir=%d target=%s:%u\n",
                               runtime_line_id(rx_line),
                               (int) forward_dir,
                               forward_host,
                               forward_port);
                        fflush(stdout);
                        }
                    }
                } else if (tun_opened) {
                    if (!xgw_tun_write(&tun, result->packet, result->packet_len)) {
                        set_error(error, error_len, "tun write failed");
                        break;
                    }
                }
            }
        }
packet_done:

        if (!did_work) {
            xgw_sleep_ms(1U);
        }
    }

cleanup:
    xgw_bridge_egress_free(bridge_egress);
    xgw_bridge_server_stop(bridge);
    xgw_outbound_close(&outbound);
    if (net_opened) {
        if (strcmp(config->transport, "afxdp") == 0) {
            xgw_afxdp_close(&xsk);
        } else {
            xgw_udp_close(&udp);
        }
    }
    if (tun_opened) {
        xgw_tun_close(&tun);
    }
    free(dp);
    free(batch);
    free(result);
    free(fec_frame);
    free(tun_buf);
    free(packet);
    free(bridge_buf);
    free(bridge_targets);
    free(stream_sched);
    return 0;
}
