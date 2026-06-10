#ifndef XGW_FRAME_H
#define XGW_FRAME_H

/* 分片、FEC、重组与帧批处理定义。 */

#include "xgw_protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define XGW_MAX_FRAME_SIZE 2048
#define XGW_MAX_FRAGMENTS 64
#define XGW_MAX_FEC_GROUPS 64

/* 分片头解析结果。 */
typedef struct xgw_fragment_info {
    uint32_t group_id;
    uint16_t index;
    uint16_t count;
} xgw_fragment_info_t;

/* FEC 头解析结果。 */
typedef struct xgw_fec_info {
    uint32_t group_id;
    uint8_t index;
    uint8_t count;
} xgw_fec_info_t;

/* 一次出站构建后得到的帧批次。 */
typedef struct xgw_frame_batch {
    uint8_t frames[XGW_MAX_FRAGMENTS][XGW_MAX_FRAME_SIZE];
    size_t frame_lengths[XGW_MAX_FRAGMENTS];
    uint8_t fragment_payloads[XGW_MAX_FRAGMENTS][XGW_MAX_FRAME_SIZE];
    size_t fragment_lengths[XGW_MAX_FRAGMENTS];
    size_t frame_count;
    uint32_t group_id;
    uint64_t last_sequence;
} xgw_frame_batch_t;

/* 重组输出结果。 */
typedef struct xgw_reassembly_result {
    uint8_t packet[XGW_MAX_FRAME_SIZE * 2];
    size_t packet_len;
    int recovered;
} xgw_reassembly_result_t;

/* 单个 FEC group 的缓存状态。 */
typedef struct xgw_fec_group {
    uint32_t group_id;
    uint16_t count;
    uint8_t fragments[XGW_MAX_FRAGMENTS][XGW_MAX_FRAME_SIZE];
    size_t fragment_lengths[XGW_MAX_FRAGMENTS];
    uint8_t fragment_present[XGW_MAX_FRAGMENTS];
    uint8_t parity[XGW_MAX_FRAME_SIZE];
    size_t parity_len;
    time_t created_at;
} xgw_fec_group_t;

/* 简化 XOR FEC codec 的运行时状态。 */
typedef struct xgw_fec_codec {
    int enabled;
    uint32_t data_shards;
    uint32_t parity_shards;
    uint32_t group_timeout_sec;
    xgw_fec_group_t groups[XGW_MAX_FEC_GROUPS];
    size_t group_count;
} xgw_fec_codec_t;

/* 初始化 FEC codec。 */
void xgw_fec_codec_init(xgw_fec_codec_t *codec, uint32_t data_shards, uint32_t parity_shards);
/* 解析分片负载。 */
int xgw_fragment_decode(const uint8_t *payload, size_t payload_len, xgw_fragment_info_t *info, const uint8_t **data, size_t *data_len);
/* 解析 FEC 负载。 */
int xgw_fec_decode(const uint8_t *payload, size_t payload_len, xgw_fec_info_t *info, const uint8_t **data, size_t *data_len);
/* 编码分片负载。 */
size_t xgw_fragment_encode(uint32_t group_id, uint16_t index, uint16_t count, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap);
/* 编码 FEC 负载。 */
size_t xgw_fec_encode(uint32_t group_id, uint8_t index, uint8_t count, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap);
/* 把 payload 构造成一组数据帧。 */
int xgw_build_data_frames(uint8_t version, uint32_t session_id, uint64_t start_seq, const uint8_t *payload, size_t payload_len, size_t max_payload_size, xgw_frame_batch_t *batch);
/* 构造一帧 FEC parity。 */
int xgw_build_fec_frame(uint8_t version, uint32_t session_id, uint64_t seq, uint32_t group_id, const xgw_frame_batch_t *batch, uint8_t *out, size_t out_cap, size_t *out_len);
/* 向 FEC 缓冲加入数据分片。 */
int xgw_fec_add_data(xgw_fec_codec_t *codec, const uint8_t *payload, size_t payload_len, xgw_reassembly_result_t *result);
/* 向 FEC 缓冲加入 parity 分片。 */
int xgw_fec_add_parity(xgw_fec_codec_t *codec, const uint8_t *payload, size_t payload_len, xgw_reassembly_result_t *result);
/* 清理超时的 FEC group。 */
void xgw_fec_reap(xgw_fec_codec_t *codec, time_t now);

#endif
