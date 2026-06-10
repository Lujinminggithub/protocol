/* 分片、FEC 与帧构造实现。 */

#include "xgw_frame.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint16_t read_be16(const uint8_t *buf) {
    return (uint16_t) (((uint16_t) buf[0] << 8U) | (uint16_t) buf[1]);
}

static uint32_t read_be32(const uint8_t *buf) {
    return ((uint32_t) buf[0] << 24U) |
           ((uint32_t) buf[1] << 16U) |
           ((uint32_t) buf[2] << 8U) |
           (uint32_t) buf[3];
}

static void write_be16(uint8_t *buf, uint16_t value) {
    buf[0] = (uint8_t) (value >> 8U);
    buf[1] = (uint8_t) value;
}

static void write_be32(uint8_t *buf, uint32_t value) {
    buf[0] = (uint8_t) (value >> 24U);
    buf[1] = (uint8_t) (value >> 16U);
    buf[2] = (uint8_t) (value >> 8U);
    buf[3] = (uint8_t) value;
}

static uint32_t seq_to_group_id(uint64_t seq) {
    return (uint32_t) (seq & 0xffffffffU);
}

static size_t clamp_payload_size(size_t max_payload_size) {
    if (max_payload_size <= XGW_FRAGMENT_HEADER_SIZE) {
        return 1200U;
    }
    return max_payload_size;
}

static void xor_parity(uint8_t *out, size_t *out_len, const xgw_frame_batch_t *batch) {
    size_t i;
    size_t j;
    size_t max_len = 0;
    memset(out, 0, XGW_MAX_FRAME_SIZE);
    for (i = 0; i < batch->frame_count; ++i) {
        if (batch->fragment_lengths[i] > max_len) {
            max_len = batch->fragment_lengths[i];
        }
    }
    for (i = 0; i < batch->frame_count; ++i) {
        for (j = 0; j < batch->fragment_lengths[i]; ++j) {
            out[j] ^= batch->fragment_payloads[i][j];
        }
    }
    *out_len = max_len;
}

static xgw_fec_group_t *find_group(xgw_fec_codec_t *codec, uint32_t group_id) {
    size_t i;
    for (i = 0; i < codec->group_count; ++i) {
        if (codec->groups[i].group_id == group_id) {
            return &codec->groups[i];
        }
    }
    return NULL;
}

static xgw_fec_group_t *ensure_group(xgw_fec_codec_t *codec, uint32_t group_id, uint16_t count) {
    xgw_fec_group_t *group = find_group(codec, group_id);
    if (group != NULL) {
        if (group->count == 0) {
            group->count = count;
        }
        return group;
    }
    if (codec->group_count >= XGW_MAX_FEC_GROUPS) {
        return NULL;
    }
    group = &codec->groups[codec->group_count++];
    memset(group, 0, sizeof(*group));
    group->group_id = group_id;
    group->count = count;
    group->created_at = time(NULL);
    return group;
}

static void drop_group(xgw_fec_codec_t *codec, xgw_fec_group_t *group) {
    size_t idx = (size_t) (group - codec->groups);
    if (idx >= codec->group_count) {
        return;
    }
    codec->groups[idx] = codec->groups[codec->group_count - 1U];
    --codec->group_count;
}

static int try_reassemble(xgw_fec_codec_t *codec, xgw_fec_group_t *group, xgw_reassembly_result_t *result) {
    size_t i;
    int missing = -1;
    size_t packet_len = 0;
    uint8_t recovered[XGW_MAX_FRAME_SIZE];
    size_t recovered_len = group->parity_len;

    if (group == NULL || group->count == 0 || group->count > XGW_MAX_FRAGMENTS) {
        return 0;
    }
    for (i = 0; i < group->count; ++i) {
        if (!group->fragment_present[i]) {
            if (missing >= 0) {
                return 0;
            }
            missing = (int) i;
        }
    }
    if (missing >= 0) {
        if (group->parity_len == 0) {
            return 0;
        }
        memcpy(recovered, group->parity, recovered_len);
        for (i = 0; i < group->count; ++i) {
            size_t j;
            if ((int) i == missing || !group->fragment_present[i]) {
                continue;
            }
            for (j = 0; j < group->fragment_lengths[i] && j < recovered_len; ++j) {
                recovered[j] ^= group->fragments[i][j];
            }
        }
        memcpy(group->fragments[missing], recovered, recovered_len);
        group->fragment_lengths[missing] = recovered_len;
        group->fragment_present[missing] = 1;
    }

    memset(result, 0, sizeof(*result));
    for (i = 0; i < group->count; ++i) {
        xgw_fragment_info_t info;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        if (!xgw_fragment_decode(group->fragments[i], group->fragment_lengths[i], &info, &payload, &payload_len)) {
            return 0;
        }
        if (packet_len + payload_len > sizeof(result->packet)) {
            return 0;
        }
        memcpy(result->packet + packet_len, payload, payload_len);
        packet_len += payload_len;
    }
    result->packet_len = packet_len;
    result->recovered = (missing >= 0);
    drop_group(codec, group);
    return 1;
}

void xgw_fec_codec_init(xgw_fec_codec_t *codec, uint32_t data_shards, uint32_t parity_shards) {
    memset(codec, 0, sizeof(*codec));
    codec->enabled = (data_shards > 0U && parity_shards > 0U);
    codec->data_shards = data_shards;
    codec->parity_shards = parity_shards;
    codec->group_timeout_sec = 5U;
}

/* 解出分片头和真实负载。 */
int xgw_fragment_decode(const uint8_t *payload, size_t payload_len, xgw_fragment_info_t *info, const uint8_t **data, size_t *data_len) {
    if (payload_len < XGW_FRAGMENT_HEADER_SIZE) {
        return 0;
    }
    info->group_id = read_be32(payload);
    info->index = read_be16(payload + 4U);
    info->count = read_be16(payload + 6U);
    *data = payload + XGW_FRAGMENT_HEADER_SIZE;
    *data_len = payload_len - XGW_FRAGMENT_HEADER_SIZE;
    return 1;
}

/* 解出 FEC 头和 parity 负载。 */
int xgw_fec_decode(const uint8_t *payload, size_t payload_len, xgw_fec_info_t *info, const uint8_t **data, size_t *data_len) {
    if (payload_len < XGW_FEC_HEADER_SIZE) {
        return 0;
    }
    info->group_id = read_be32(payload);
    info->index = payload[4];
    info->count = payload[5];
    *data = payload + XGW_FEC_HEADER_SIZE;
    *data_len = payload_len - XGW_FEC_HEADER_SIZE;
    return 1;
}

size_t xgw_fragment_encode(uint32_t group_id, uint16_t index, uint16_t count, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap) {
    if (out_cap < XGW_FRAGMENT_HEADER_SIZE + payload_len) {
        return 0;
    }
    write_be32(out, group_id);
    write_be16(out + 4U, index);
    write_be16(out + 6U, count);
    memcpy(out + XGW_FRAGMENT_HEADER_SIZE, payload, payload_len);
    return XGW_FRAGMENT_HEADER_SIZE + payload_len;
}

size_t xgw_fec_encode(uint32_t group_id, uint8_t index, uint8_t count, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap) {
    if (out_cap < XGW_FEC_HEADER_SIZE + payload_len) {
        return 0;
    }
    write_be32(out, group_id);
    out[4] = index;
    out[5] = count;
    memcpy(out + XGW_FEC_HEADER_SIZE, payload, payload_len);
    return XGW_FEC_HEADER_SIZE + payload_len;
}

int xgw_build_data_frames(uint8_t version, uint32_t session_id, uint64_t start_seq, const uint8_t *payload, size_t payload_len, size_t max_payload_size, xgw_frame_batch_t *batch) {
    size_t data_budget;
    size_t fragment_count;
    size_t i;
    uint64_t seq = start_seq;
    memset(batch, 0, sizeof(*batch));
    data_budget = clamp_payload_size(max_payload_size) - XGW_FRAGMENT_HEADER_SIZE;
    if (data_budget == 0) {
        return 0;
    }
    fragment_count = (payload_len == 0U) ? 1U : ((payload_len + data_budget - 1U) / data_budget);
    if (fragment_count > XGW_MAX_FRAGMENTS) {
        return 0;
    }
    batch->frame_count = fragment_count;
    batch->group_id = seq_to_group_id(start_seq);
    for (i = 0; i < fragment_count; ++i) {
        size_t offset = i * data_budget;
        size_t piece_len = 0;
        xgw_header_t header;
        size_t frag_len;
        size_t out_len;
        if (offset < payload_len) {
            piece_len = payload_len - offset;
            if (piece_len > data_budget) {
                piece_len = data_budget;
            }
        }
        frag_len = xgw_fragment_encode(batch->group_id,
                                       (uint16_t) i,
                                       (uint16_t) fragment_count,
                                       payload + offset,
                                       piece_len,
                                       batch->fragment_payloads[i],
                                       sizeof(batch->fragment_payloads[i]));
        if (frag_len == 0) {
            return 0;
        }
        batch->fragment_lengths[i] = frag_len;
        memset(&header, 0, sizeof(header));
        header.version = version;
        header.type = XGW_MESSAGE_DATA;
        header.session_id = session_id;
        header.sequence = seq++;
        header.payload_length = (uint16_t) frag_len;
        if (!xgw_header_encode(&header,
                               batch->fragment_payloads[i],
                               frag_len,
                               batch->frames[i],
                               sizeof(batch->frames[i]),
                               &out_len)) {
            return 0;
        }
        batch->frame_lengths[i] = out_len;
    }
    batch->last_sequence = seq - 1U;
    return 1;
}

/* 生成一帧 parity FEC。 */
int xgw_build_fec_frame(uint8_t version, uint32_t session_id, uint64_t seq, uint32_t group_id, const xgw_frame_batch_t *batch, uint8_t *out, size_t out_cap, size_t *out_len) {
    uint8_t parity[XGW_MAX_FRAME_SIZE];
    size_t parity_len = 0;
    uint8_t payload[XGW_MAX_FRAME_SIZE];
    size_t payload_len;
    xgw_header_t header;
    xor_parity(parity, &parity_len, batch);
    payload_len = xgw_fec_encode(group_id,
                                 (uint8_t) batch->frame_count,
                                 (uint8_t) (batch->frame_count + 1U),
                                 parity,
                                 parity_len,
                                 payload,
                                 sizeof(payload));
    if (payload_len == 0) {
        return 0;
    }
    memset(&header, 0, sizeof(header));
    header.version = version;
    header.type = XGW_MESSAGE_FEC;
    header.session_id = session_id;
    header.sequence = seq;
    header.payload_length = (uint16_t) payload_len;
    return xgw_header_encode(&header, payload, payload_len, out, out_cap, out_len);
}

/* 添加一个数据分片，并在可能时触发重组。 */
int xgw_fec_add_data(xgw_fec_codec_t *codec, const uint8_t *payload, size_t payload_len, xgw_reassembly_result_t *result) {
    xgw_fragment_info_t info;
    xgw_fec_group_t *group;
    const uint8_t *fragment_payload = NULL;
    size_t fragment_payload_len = 0;
    if (!codec->enabled) {
        return 0;
    }
    if (!xgw_fragment_decode(payload, payload_len, &info, &fragment_payload, &fragment_payload_len)) {
        return 0;
    }
    if (info.count == 0 || info.index >= info.count || info.count > XGW_MAX_FRAGMENTS) {
        return 0;
    }
    group = ensure_group(codec, info.group_id, info.count);
    if (group == NULL) {
        return 0;
    }
    memcpy(group->fragments[info.index], payload, payload_len);
    group->fragment_lengths[info.index] = payload_len;
    group->fragment_present[info.index] = 1;
    return try_reassemble(codec, group, result);
}

/* 添加一个 FEC parity 分片，并在可能时触发重组。 */
int xgw_fec_add_parity(xgw_fec_codec_t *codec, const uint8_t *payload, size_t payload_len, xgw_reassembly_result_t *result) {
    xgw_fec_info_t info;
    const uint8_t *parity_payload = NULL;
    size_t parity_payload_len = 0;
    xgw_fec_group_t *group;
    if (!codec->enabled) {
        return 0;
    }
    if (!xgw_fec_decode(payload, payload_len, &info, &parity_payload, &parity_payload_len)) {
        return 0;
    }
    group = ensure_group(codec, info.group_id, (uint16_t) (info.count - 1U));
    if (group == NULL) {
        return 0;
    }
    memcpy(group->parity, parity_payload, parity_payload_len);
    group->parity_len = parity_payload_len;
    return try_reassemble(codec, group, result);
}

/* 清理超时的 FEC group。 */
void xgw_fec_reap(xgw_fec_codec_t *codec, time_t now) {
    size_t i = 0;
    while (i < codec->group_count) {
        if ((uint32_t) (now - codec->groups[i].created_at) > codec->group_timeout_sec) {
            codec->groups[i] = codec->groups[codec->group_count - 1U];
            --codec->group_count;
            continue;
        }
        ++i;
    }
}
