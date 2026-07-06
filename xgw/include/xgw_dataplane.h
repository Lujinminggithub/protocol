#ifndef XGW_DATAPLANE_H
#define XGW_DATAPLANE_H

#include "xgw_acl.h"
#include "xgw_config.h"
#include "xgw_coremodel.h"
#include "xgw_frame.h"
#include "xgw_pool.h"
#include "xgw_policy.h"
#include "xgw_session.h"
#include "xgw_transport.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 路径 MTU 发现（PMTUD）运行时状态。
 *
 * 思路：以「有效负载字节数」effective_payload 为搜索量，DF 位已在 UDP socket 上打开。
 * - 发送遇到 EMSGSIZE（报文过大）→ 立即回退到 floor 并标记需要重新探测；
 * - 内核 IP_MTU 报告了更小的 PMTU → 据此把 effective 钳到安全值；
 * - 一段时间无「过大」事件 → 周期性向上探测，逐步逼近 ceiling（配置 payload）。
 * 整段链路逐跳生效（每个节点对自己的下一跳/上一跳各自发现）。
 */
typedef struct xgw_pmtu_state {
    int enabled;
    uint32_t effective_payload;   /* 当前允许的最大应用负载（字节） */
    uint32_t floor_payload;       /* 探测下限（保守安全值） */
    uint32_t ceiling_payload;     /* 探测上限（=配置 payload_size） */
    uint32_t overhead_bytes;      /* 帧头+加封等固定开销，用于 PMTU→payload 换算 */
    uint32_t probe_step;          /* 每次向上探测的步长 */
    uint64_t last_too_big_us;     /* 上次遇到「过大」的时间 */
    uint64_t last_probe_us;       /* 上次向上探测的时间 */
    uint32_t too_big_events;      /* 累计「过大」事件数 */
} xgw_pmtu_state_t;

typedef struct xgw_dataplane {
    xgw_runtime_config_t config;
    xgw_negotiation_info_t negotiation;
    xgw_acl_engine_t acl;
    xgw_pool_t pool;
    xgw_session_table_t sessions;
    xgw_stream_xport_pool_t streams;
    xgw_fec_codec_t fec;
    xgw_dos_protector_t dos;
    xgw_flow_hint_t current_flow_hint;
    xgw_pmtu_state_t pmtu;
} xgw_dataplane_t;

void xgw_dataplane_init(xgw_dataplane_t *dp, const xgw_runtime_config_t *config);
int xgw_dataplane_process_frame(xgw_dataplane_t *dp,
                                const xgw_packet_t *packet,
                                xgw_reassembly_result_t *result,
                                char *summary,
                                size_t summary_len);
int xgw_dataplane_build_outbound(xgw_dataplane_t *dp,
                                 xgw_session_t *session,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 xgw_frame_batch_t *batch,
                                 uint8_t *fec_frame,
                                 size_t fec_cap,
                                 size_t *fec_len);
void xgw_dataplane_set_flow_hint(xgw_dataplane_t *dp, const xgw_flow_hint_t *hint);
const char *xgw_dataplane_last_error(void);

/* PMTUD：初始化（从 profile 推导上下限）。 */
void xgw_dataplane_pmtu_init(xgw_dataplane_t *dp);
/* 当前有效应用负载（字节）；PMTUD 关闭时返回配置 payload_size。 */
uint32_t xgw_dataplane_effective_payload(const xgw_dataplane_t *dp);
/* 报告一次发送结果：too_big 非 0 表示遇到 EMSGSIZE，需回退。 */
void xgw_dataplane_pmtu_note_send(xgw_dataplane_t *dp, int too_big, uint64_t now_us);
/* 周期性维护：传入内核发现的 PMTU（字节，0=未知），按需向上探测。 */
void xgw_dataplane_pmtu_tick(xgw_dataplane_t *dp, uint32_t kernel_pmtu, uint64_t now_us);

#endif
