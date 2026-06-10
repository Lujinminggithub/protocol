#ifndef XGW_DATAPLANE_H
#define XGW_DATAPLANE_H

/* 数据面聚合对象：把配置、session、FEC 和 DOS 防护放到同一个运行上下文中。 */

#include "xgw_config.h"
#include "xgw_acl.h"
#include "xgw_frame.h"
#include "xgw_pool.h"
#include "xgw_policy.h"
#include "xgw_session.h"
#include "xgw_transport.h"

#include <stddef.h>
#include <stdint.h>

/* 单个节点的核心数据面状态。 */
typedef struct xgw_dataplane {
    xgw_runtime_config_t config;
    xgw_negotiation_info_t negotiation;
    xgw_acl_engine_t acl;
    xgw_pool_t pool;
    xgw_session_table_t sessions;
    xgw_fec_codec_t fec;
    xgw_dos_protector_t dos;
} xgw_dataplane_t;

/* 初始化数据面对象。 */
void xgw_dataplane_init(xgw_dataplane_t *dp, const xgw_runtime_config_t *config);
/* 处理一帧入站数据，并在需要时给出重组结果。 */
int xgw_dataplane_process_frame(xgw_dataplane_t *dp, const xgw_packet_t *packet, xgw_reassembly_result_t *result, char *summary, size_t summary_len);
/* 把一段明文 payload 组装为出站帧和可选 FEC 帧。 */
int xgw_dataplane_build_outbound(xgw_dataplane_t *dp, xgw_session_t *session, const uint8_t *payload, size_t payload_len, xgw_frame_batch_t *batch, uint8_t *fec_frame, size_t fec_cap, size_t *fec_len);
/* 返回最近一次出站构建失败的原因。 */
const char *xgw_dataplane_last_error(void);

#endif
