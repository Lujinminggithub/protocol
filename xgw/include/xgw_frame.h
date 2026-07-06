#ifndef XGW_FRAME_H
#define XGW_FRAME_H

/* 分片、FEC、重组与帧批处理定义。 */

#include "xgw_protocol.h"
#include "xgw_coremodel.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define XGW_MAX_FRAME_SIZE 2048
#define XGW_MAX_FRAGMENTS 64
#define XGW_MAX_FEC_GROUPS 64
#define XGW_MAX_COMPLETED_FEC_GROUPS 128
#define XGW_MAX_FEC_PARITY 8

typedef struct xgw_fragment_info {
    uint32_t group_id;
    uint16_t index;
    uint16_t count;
    uint16_t payload_len;
} xgw_fragment_info_t;

typedef struct xgw_fec_info {
    uint32_t group_id;
    uint8_t index;
    uint8_t data_count;
    uint8_t parity_count;
    uint8_t scheme;
    uint16_t payload_len;
} xgw_fec_info_t;

typedef struct xgw_frame_batch {
    uint8_t frames[XGW_MAX_FRAGMENTS + XGW_MAX_FEC_PARITY][XGW_MAX_FRAME_SIZE];
    size_t frame_lengths[XGW_MAX_FRAGMENTS + XGW_MAX_FEC_PARITY];
    uint8_t fragment_payloads[XGW_MAX_FRAGMENTS][XGW_MAX_FRAME_SIZE];
    size_t fragment_lengths[XGW_MAX_FRAGMENTS];
    size_t frame_count;
    size_t data_frame_count;
    size_t fec_frame_count;
    uint32_t group_id;
    uint64_t last_sequence;
    xgw_flow_hint_t flow_hint;
} xgw_frame_batch_t;

typedef struct xgw_reassembly_result {
    uint8_t packet[XGW_MAX_FRAME_SIZE * 4];
    size_t packet_len;
    int recovered;
    uint32_t session_id;
    uint8_t control_frame[XGW_MAX_FRAME_SIZE];
    size_t control_frame_len;
} xgw_reassembly_result_t;

typedef struct xgw_fec_group {
    uint32_t group_id;
    uint32_t session_id;
    char peer_host[64];
    uint16_t peer_port;
    uint16_t count;
    uint8_t parity_count;
    uint8_t fragments[XGW_MAX_FRAGMENTS][XGW_MAX_FRAME_SIZE];
    size_t fragment_lengths[XGW_MAX_FRAGMENTS];
    uint8_t fragment_present[XGW_MAX_FRAGMENTS];
    uint8_t parity[XGW_MAX_FEC_PARITY][XGW_MAX_FRAME_SIZE];
    size_t parity_lengths[XGW_MAX_FEC_PARITY];
    uint8_t parity_present[XGW_MAX_FEC_PARITY];
    time_t created_at;
} xgw_fec_group_t;

typedef struct xgw_completed_fec_group {
    uint32_t group_id;
    uint32_t session_id;
    char peer_host[64];
    uint16_t peer_port;
    time_t completed_at;
} xgw_completed_fec_group_t;

typedef struct xgw_fec_codec {
    int enabled;
    uint32_t data_shards;
    uint32_t parity_shards;
    uint32_t group_timeout_sec;
    xgw_fec_group_t groups[XGW_MAX_FEC_GROUPS];
    size_t group_count;
    xgw_completed_fec_group_t completed[XGW_MAX_COMPLETED_FEC_GROUPS];
    size_t completed_count;
    size_t completed_cursor;
} xgw_fec_codec_t;

void xgw_fec_codec_init(xgw_fec_codec_t *codec, uint32_t data_shards, uint32_t parity_shards);
int xgw_fragment_decode(const uint8_t *payload, size_t payload_len, xgw_fragment_info_t *info, const uint8_t **data, size_t *data_len);
int xgw_fec_decode(const uint8_t *payload, size_t payload_len, xgw_fec_info_t *info, const uint8_t **data, size_t *data_len);
size_t xgw_fragment_encode(uint32_t group_id, uint16_t index, uint16_t count, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap);
size_t xgw_fec_encode(uint32_t group_id,
                      uint8_t index,
                      uint8_t data_count,
                      uint8_t parity_count,
                      uint8_t scheme,
                      const uint8_t *payload,
                      size_t payload_len,
                      uint8_t *out,
                      size_t out_cap);
int xgw_build_data_frames(uint8_t version,
                          uint32_t session_id,
                          uint32_t stream_id,
                          uint64_t start_seq,
                          const uint8_t *payload,
                          size_t payload_len,
                          size_t max_payload_size,
                          uint16_t flags,
                          xgw_frame_batch_t *batch);
int xgw_build_fec_frames(uint8_t version,
                         uint32_t session_id,
                         uint32_t stream_id,
                         uint64_t start_seq,
                         const xgw_frame_batch_t *batch,
                         uint32_t parity_count,
                         uint16_t flags,
                         uint8_t frames[][XGW_MAX_FRAME_SIZE],
                         size_t *frame_lengths,
                         size_t frame_cap,
                         size_t *out_count,
                         uint64_t *last_sequence);
int xgw_fec_add_data(xgw_fec_codec_t *codec,
                     uint32_t session_id,
                     const char *peer_host,
                     uint16_t peer_port,
                     const uint8_t *payload,
                     size_t payload_len,
                     xgw_reassembly_result_t *result);
int xgw_fec_add_parity(xgw_fec_codec_t *codec,
                       uint32_t session_id,
                       const char *peer_host,
                       uint16_t peer_port,
                       const uint8_t *payload,
                       size_t payload_len,
                       xgw_reassembly_result_t *result);
void xgw_fec_reap(xgw_fec_codec_t *codec, time_t now);

#endif
