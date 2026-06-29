/* 分片、GF(256)/RS FEC 与帧构造实现。 */

#include "xgw_frame.h"

#include <string.h>
#include <stdio.h>

#define XGW_GF_POLY 0x1dU

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_ready = 0;

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

/* group_id 必须 per-(stream,batch) 唯一:per-stream 序号空间下不同 stream 都从小序号起,
 * 仅用 seq 会让不同 stream 生成相同 group_id → FEC 重组把不同 stream 的 fragment 混进同一组 → 串扰。
 * 混入 stream_id(高 16 位)+ seq(低 16 位)区分;同 stream 相邻 batch 靠 seq 低位区分。 */
static uint32_t seq_to_group_id(uint32_t stream_id, uint64_t seq) {
    return ((stream_id & 0xffffU) << 16) | (uint32_t) (seq & 0xffffU);
}

static size_t clamp_payload_size(size_t max_payload_size) {
    if (max_payload_size <= XGW_FRAGMENT_HEADER_SIZE) {
        return 1200U;
    }
    return max_payload_size;
}

static void gf_init(void) {
    uint16_t x = 1U;
    int i;
    if (gf_ready) {
        return;
    }
    gf_exp[0] = 1U;
    for (i = 1; i < 255; ++i) {
        x <<= 1U;
        if ((x & 0x100U) != 0U) {
            x ^= XGW_GF_POLY;
        }
        gf_exp[i] = (uint8_t) x;
    }
    for (i = 255; i < 512; ++i) {
        gf_exp[i] = gf_exp[i - 255];
    }
    gf_log[0] = 0U;
    for (i = 0; i < 255; ++i) {
        gf_log[gf_exp[i]] = (uint8_t) i;
    }
    gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0U || b == 0U) {
        return 0U;
    }
    return gf_exp[(int) gf_log[a] + (int) gf_log[b]];
}

static uint8_t gf_inv(uint8_t a) {
    if (a == 0U) {
        return 0U;
    }
    return gf_exp[255 - gf_log[a]];
}

static uint8_t gf_pow_base(uint8_t power) {
    return gf_exp[power % 255U];
}

static void rs_build_matrix(uint8_t *matrix, size_t rows, size_t cols) {
    size_t r;
    size_t c;
    for (r = 0; r < rows; ++r) {
        if (r < cols) {
            for (c = 0; c < cols; ++c) {
                matrix[r * cols + c] = (uint8_t) (c == r ? 1U : 0U);
            }
        } else {
            uint8_t base = gf_pow_base((uint8_t) (r - cols + 1U));
            uint8_t val = 1U;
            for (c = 0; c < cols; ++c) {
                matrix[r * cols + c] = val;
                val = gf_mul(val, base);
            }
        }
    }
}

static int gf_matrix_invert(const uint8_t *input, uint8_t *output, size_t n) {
    uint8_t aug[XGW_MAX_FRAGMENTS * XGW_MAX_FRAGMENTS * 2];
    size_t r;
    size_t c;
    size_t pivot;
    if (n == 0U || n > XGW_MAX_FRAGMENTS) {
        return 0;
    }
    memset(aug, 0, sizeof(aug));
    for (r = 0; r < n; ++r) {
        for (c = 0; c < n; ++c) {
            aug[r * (2U * n) + c] = input[r * n + c];
        }
        aug[r * (2U * n) + n + r] = 1U;
    }
    for (pivot = 0; pivot < n; ++pivot) {
        size_t best = pivot;
        uint8_t inv;
        if (aug[best * (2U * n) + pivot] == 0U) {
            for (r = pivot + 1U; r < n; ++r) {
                if (aug[r * (2U * n) + pivot] != 0U) {
                    best = r;
                    break;
                }
            }
            if (aug[best * (2U * n) + pivot] == 0U) {
                return 0;
            }
            for (c = 0; c < 2U * n; ++c) {
                uint8_t tmp = aug[pivot * (2U * n) + c];
                aug[pivot * (2U * n) + c] = aug[best * (2U * n) + c];
                aug[best * (2U * n) + c] = tmp;
            }
        }
        inv = gf_inv(aug[pivot * (2U * n) + pivot]);
        for (c = 0; c < 2U * n; ++c) {
            aug[pivot * (2U * n) + c] = gf_mul(aug[pivot * (2U * n) + c], inv);
        }
        for (r = 0; r < n; ++r) {
            uint8_t factor;
            if (r == pivot) {
                continue;
            }
            factor = aug[r * (2U * n) + pivot];
            if (factor == 0U) {
                continue;
            }
            for (c = 0; c < 2U * n; ++c) {
                aug[r * (2U * n) + c] ^= gf_mul(factor, aug[pivot * (2U * n) + c]);
            }
        }
    }
    for (r = 0; r < n; ++r) {
        for (c = 0; c < n; ++c) {
            output[r * n + c] = aug[r * (2U * n) + n + c];
        }
    }
    return 1;
}

static xgw_fec_group_t *find_group(xgw_fec_codec_t *codec,
                                   uint32_t group_id,
                                   uint32_t session_id,
                                   const char *peer_host,
                                   uint16_t peer_port) {
    size_t i;
    const char *host = peer_host == NULL ? "" : peer_host;
    for (i = 0; i < codec->group_count; ++i) {
        if (codec->groups[i].group_id == group_id &&
            codec->groups[i].session_id == session_id &&
            strcmp(codec->groups[i].peer_host, host) == 0 &&
            codec->groups[i].peer_port == peer_port) {
            return &codec->groups[i];
        }
    }
    return NULL;
}

static int is_completed_group(const xgw_fec_codec_t *codec,
                              uint32_t group_id,
                              uint32_t session_id,
                              const char *peer_host,
                              uint16_t peer_port) {
    size_t i;
    const char *host = peer_host == NULL ? "" : peer_host;
    if (codec == NULL) {
        return 0;
    }
    for (i = 0; i < codec->completed_count; ++i) {
        const xgw_completed_fec_group_t *entry = &codec->completed[i];
        if (entry->group_id == group_id &&
            entry->session_id == session_id &&
            entry->peer_port == peer_port &&
            strcmp(entry->peer_host, host) == 0) {
            return 1;
        }
    }
    return 0;
}

static void remember_completed_group(xgw_fec_codec_t *codec,
                                     uint32_t group_id,
                                     uint32_t session_id,
                                     const char *peer_host,
                                     uint16_t peer_port) {
    xgw_completed_fec_group_t *entry;
    const char *host = peer_host == NULL ? "" : peer_host;
    if (codec == NULL || group_id == 0U) {
        return;
    }
    if (is_completed_group(codec, group_id, session_id, host, peer_port)) {
        return;
    }
    if (codec->completed_count < XGW_MAX_COMPLETED_FEC_GROUPS) {
        entry = &codec->completed[codec->completed_count++];
    } else {
        entry = &codec->completed[codec->completed_cursor++ % XGW_MAX_COMPLETED_FEC_GROUPS];
    }
    memset(entry, 0, sizeof(*entry));
    entry->group_id = group_id;
    entry->session_id = session_id;
    snprintf(entry->peer_host, sizeof(entry->peer_host), "%s", host);
    entry->peer_port = peer_port;
    entry->completed_at = time(NULL);
}

static void reap_stale_groups(xgw_fec_codec_t *codec, time_t now) {
    size_t i = 0U;
    while (codec != NULL && i < codec->group_count) {
        if ((uint32_t) (now - codec->groups[i].created_at) > codec->group_timeout_sec) {
            codec->groups[i] = codec->groups[codec->group_count - 1U];
            codec->group_count--;
            continue;
        }
        ++i;
    }
}

static xgw_fec_group_t *ensure_group(xgw_fec_codec_t *codec,
                                     uint32_t group_id,
                                     uint32_t session_id,
                                     const char *peer_host,
                                     uint16_t peer_port,
                                     uint16_t count,
                                     uint8_t parity_count) {
    xgw_fec_group_t *group = find_group(codec, group_id, session_id, peer_host, peer_port);
    if (group != NULL) {
        if (group->count == 0U) {
            group->count = count;
        }
        if (group->parity_count == 0U) {
            group->parity_count = parity_count;
        }
        return group;
    }
    if (codec->group_count >= XGW_MAX_FEC_GROUPS) {
        reap_stale_groups(codec, time(NULL));
        if (codec->group_count >= XGW_MAX_FEC_GROUPS) {
            return NULL;
        }
    }
    group = &codec->groups[codec->group_count++];
    memset(group, 0, sizeof(*group));
    group->group_id = group_id;
    group->session_id = session_id;
    snprintf(group->peer_host, sizeof(group->peer_host), "%s", peer_host == NULL ? "" : peer_host);
    group->peer_port = peer_port;
    group->count = count;
    group->parity_count = parity_count;
    group->created_at = time(NULL);
    return group;
}

static void drop_group(xgw_fec_codec_t *codec, xgw_fec_group_t *group) {
    size_t idx = (size_t) (group - codec->groups);
    if (idx < codec->group_count) {
        codec->groups[idx] = codec->groups[codec->group_count - 1U];
        codec->group_count--;
    }
}

static int reassemble_packet(xgw_fec_group_t *group, xgw_reassembly_result_t *result) {
    size_t i;
    size_t packet_len = 0U;
    memset(result, 0, sizeof(*result));
    for (i = 0; i < group->count; ++i) {
        xgw_fragment_info_t info;
        const uint8_t *payload = NULL;
        size_t payload_len = 0U;
        if (!group->fragment_present[i]) {
            return 0;
        }
        if (!xgw_fragment_decode(group->fragments[i], group->fragment_lengths[i], &info, &payload, &payload_len)) {
            return 0;
        }
        if (info.index != i || info.count != group->count) {
            return 0;
        }
        if (packet_len + payload_len > sizeof(result->packet)) {
            return 0;
        }
        memcpy(result->packet + packet_len, payload, payload_len);
        packet_len += payload_len;
    }
    result->packet_len = packet_len;
    return 1;
}

static int try_rs_recover(xgw_fec_group_t *group, xgw_reassembly_result_t *result) {
    size_t missing_count = 0U;
    size_t total_needed = group->count;
    size_t selected_rows[XGW_MAX_FRAGMENTS];
    uint8_t selected_payloads[XGW_MAX_FRAGMENTS][XGW_MAX_FRAME_SIZE];
    uint8_t matrix[(XGW_MAX_FRAGMENTS + XGW_MAX_FEC_PARITY) * XGW_MAX_FRAGMENTS];
    uint8_t inverse[XGW_MAX_FRAGMENTS * XGW_MAX_FRAGMENTS];
    uint8_t decode_rows[XGW_MAX_FRAGMENTS * XGW_MAX_FRAGMENTS];
    uint16_t expected_payload_len[XGW_MAX_FRAGMENTS];
    size_t i;
    size_t row = 0U;
    size_t max_len = 0U;

    memset(expected_payload_len, 0, sizeof(expected_payload_len));
    for (i = 0; i < group->count; ++i) {
        if (group->fragment_present[i]) {
            xgw_fragment_info_t info;
            const uint8_t *data = NULL;
            size_t data_len = 0U;
            if (!xgw_fragment_decode(group->fragments[i], group->fragment_lengths[i], &info, &data, &data_len)) {
                return 0;
            }
            expected_payload_len[i] = info.payload_len;
            if (group->fragment_lengths[i] > max_len) {
                max_len = group->fragment_lengths[i];
            }
        } else {
            missing_count++;
        }
    }
    if (missing_count == 0U) {
        return reassemble_packet(group, result);
    }
    if (missing_count > group->parity_count) {
        return 0;
    }

    for (i = 0; i < group->count && row < total_needed; ++i) {
        if (group->fragment_present[i]) {
            selected_rows[row] = i;
            memcpy(selected_payloads[row], group->fragments[i], group->fragment_lengths[i]);
            memset(selected_payloads[row] + group->fragment_lengths[i], 0, XGW_MAX_FRAME_SIZE - group->fragment_lengths[i]);
            row++;
        }
    }
    for (i = 0; i < group->parity_count && row < total_needed; ++i) {
        if (group->parity_present[i]) {
            selected_rows[row] = group->count + i;
            memcpy(selected_payloads[row], group->parity[i], group->parity_lengths[i]);
            memset(selected_payloads[row] + group->parity_lengths[i], 0, XGW_MAX_FRAME_SIZE - group->parity_lengths[i]);
            row++;
        }
    }
    if (row < total_needed) {
        return 0;
    }

    rs_build_matrix(matrix, group->count + group->parity_count, group->count);
    for (i = 0; i < group->count; ++i) {
        size_t c;
        for (c = 0; c < group->count; ++c) {
            decode_rows[i * group->count + c] = matrix[selected_rows[i] * group->count + c];
        }
    }
    if (!gf_matrix_invert(decode_rows, inverse, group->count)) {
        return 0;
    }

    for (i = 0; i < group->count; ++i) {
        size_t c;
        size_t b;
        memset(group->fragments[i], 0, sizeof(group->fragments[i]));
        for (c = 0; c < group->count; ++c) {
            uint8_t coef = inverse[i * group->count + c];
            if (coef == 0U) {
                continue;
            }
            for (b = 0; b < max_len; ++b) {
                group->fragments[i][b] ^= gf_mul(coef, selected_payloads[c][b]);
            }
        }
        if (expected_payload_len[i] == 0U) {
            xgw_fragment_info_t info;
            const uint8_t *data = NULL;
            size_t data_len = 0U;
            if (!xgw_fragment_decode(group->fragments[i], max_len, &info, &data, &data_len)) {
                return 0;
            }
            expected_payload_len[i] = info.payload_len;
        }
        group->fragment_lengths[i] = XGW_FRAGMENT_HEADER_SIZE + expected_payload_len[i];
        group->fragment_present[i] = 1U;
    }
    result->recovered = 1;
    return reassemble_packet(group, result);
}

static int try_reassemble(xgw_fec_codec_t *codec, xgw_fec_group_t *group, xgw_reassembly_result_t *result) {
    size_t present = 0U;
    size_t i;
    for (i = 0; i < group->count; ++i) {
        if (group->fragment_present[i]) {
            present++;
        }
    }
    if (present == group->count) {
        if (reassemble_packet(group, result)) {
            remember_completed_group(codec,
                                     group->group_id,
                                     group->session_id,
                                     group->peer_host,
                                     group->peer_port);
            drop_group(codec, group);
            return 1;
        }
        return 0;
    }
    if (present + group->parity_count >= group->count) {
        if (try_rs_recover(group, result)) {
            remember_completed_group(codec,
                                     group->group_id,
                                     group->session_id,
                                     group->peer_host,
                                     group->peer_port);
            drop_group(codec, group);
            return 1;
        }
    }
    return 0;
}

void xgw_fec_codec_init(xgw_fec_codec_t *codec, uint32_t data_shards, uint32_t parity_shards) {
    gf_init();
    memset(codec, 0, sizeof(*codec));
    codec->enabled = (data_shards > 0U && parity_shards > 0U);
    codec->data_shards = data_shards;
    codec->parity_shards = parity_shards > XGW_MAX_FEC_PARITY ? XGW_MAX_FEC_PARITY : parity_shards;
    codec->group_timeout_sec = 5U;
}

int xgw_fragment_decode(const uint8_t *payload, size_t payload_len, xgw_fragment_info_t *info, const uint8_t **data, size_t *data_len) {
    if (payload == NULL || info == NULL || data == NULL || data_len == NULL || payload_len < XGW_FRAGMENT_HEADER_SIZE) {
        return 0;
    }
    info->group_id = read_be32(payload);
    info->index = read_be16(payload + 4U);
    info->count = read_be16(payload + 6U);
    info->payload_len = read_be16(payload + 8U);
    if (payload_len < XGW_FRAGMENT_HEADER_SIZE + (size_t) info->payload_len) {
        return 0;
    }
    *data = payload + XGW_FRAGMENT_HEADER_SIZE;
    *data_len = info->payload_len;
    return 1;
}

int xgw_fec_decode(const uint8_t *payload, size_t payload_len, xgw_fec_info_t *info, const uint8_t **data, size_t *data_len) {
    if (payload == NULL || info == NULL || data == NULL || data_len == NULL || payload_len < XGW_FEC_HEADER_SIZE) {
        return 0;
    }
    info->group_id = read_be32(payload);
    info->index = payload[4];
    info->data_count = payload[5];
    info->parity_count = payload[6];
    info->scheme = payload[7];
    info->payload_len = read_be16(payload + 8U);
    if (payload_len < (size_t) XGW_FEC_HEADER_SIZE + (size_t) info->payload_len) {
        return 0;
    }
    *data = payload + XGW_FEC_HEADER_SIZE;
    *data_len = info->payload_len;
    return 1;
}

size_t xgw_fragment_encode(uint32_t group_id, uint16_t index, uint16_t count, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_cap) {
    if (out == NULL || payload == NULL || out_cap < XGW_FRAGMENT_HEADER_SIZE + payload_len) {
        return 0U;
    }
    write_be32(out, group_id);
    write_be16(out + 4U, index);
    write_be16(out + 6U, count);
    write_be16(out + 8U, (uint16_t) payload_len);
    memcpy(out + XGW_FRAGMENT_HEADER_SIZE, payload, payload_len);
    return XGW_FRAGMENT_HEADER_SIZE + payload_len;
}

size_t xgw_fec_encode(uint32_t group_id,
                      uint8_t index,
                      uint8_t data_count,
                      uint8_t parity_count,
                      uint8_t scheme,
                      const uint8_t *payload,
                      size_t payload_len,
                      uint8_t *out,
                      size_t out_cap) {
    if (out == NULL || payload == NULL || out_cap < XGW_FEC_HEADER_SIZE + payload_len) {
        return 0U;
    }
    write_be32(out, group_id);
    out[4] = index;
    out[5] = data_count;
    out[6] = parity_count;
    out[7] = scheme;
    write_be16(out + 8U, (uint16_t) payload_len);
    write_be16(out + 10U, 0U);
    memcpy(out + XGW_FEC_HEADER_SIZE, payload, payload_len);
    return XGW_FEC_HEADER_SIZE + payload_len;
}

int xgw_build_data_frames(uint8_t version,
                          uint32_t session_id,
                          uint32_t stream_id,
                          uint64_t start_seq,
                          const uint8_t *payload,
                          size_t payload_len,
                          size_t max_payload_size,
                          uint16_t flags,
                          xgw_frame_batch_t *batch) {
    size_t data_budget;
    size_t fragment_count;
    size_t i;
    uint64_t seq = start_seq;
    if (batch == NULL || payload == NULL) {
        return 0;
    }
    memset(batch, 0, sizeof(*batch));
    data_budget = clamp_payload_size(max_payload_size) - XGW_FRAGMENT_HEADER_SIZE;
    if (data_budget == 0U) {
        return 0;
    }
    fragment_count = payload_len == 0U ? 1U : ((payload_len + data_budget - 1U) / data_budget);
    if (fragment_count > XGW_MAX_FRAGMENTS) {
        return 0;
    }
    batch->frame_count = fragment_count;
    batch->data_frame_count = fragment_count;
    batch->group_id = seq_to_group_id(stream_id, start_seq);
    for (i = 0; i < fragment_count; ++i) {
        size_t offset = i * data_budget;
        size_t piece_len = offset < payload_len ? payload_len - offset : 0U;
        xgw_header_t header;
        size_t frag_len;
        size_t out_len;
        if (piece_len > data_budget) {
            piece_len = data_budget;
        }
        frag_len = xgw_fragment_encode(batch->group_id,
                                       (uint16_t) i,
                                       (uint16_t) fragment_count,
                                       payload + offset,
                                       piece_len,
                                       batch->fragment_payloads[i],
                                       sizeof(batch->fragment_payloads[i]));
        if (frag_len == 0U) {
            return 0;
        }
        batch->fragment_lengths[i] = frag_len;
        memset(&header, 0, sizeof(header));
        header.version = version;
        header.type = XGW_MESSAGE_DATA;
        header.flags = flags;
        header.session_id = session_id;
        header.stream_id = stream_id;
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
                         uint64_t *last_sequence) {
    uint64_t seq = start_seq;
    uint8_t matrix[(XGW_MAX_FRAGMENTS + XGW_MAX_FEC_PARITY) * XGW_MAX_FRAGMENTS];
    size_t max_len = 0U;
    size_t i;
    size_t p;
    if (batch == NULL || frames == NULL || frame_lengths == NULL || out_count == NULL || last_sequence == NULL) {
        return 0;
    }
    if (parity_count == 0U) {
        *out_count = 0U;
        *last_sequence = start_seq;
        return 1;
    }
    if (parity_count > frame_cap || parity_count > XGW_MAX_FEC_PARITY) {
        return 0;
    }
    rs_build_matrix(matrix, batch->data_frame_count + parity_count, batch->data_frame_count);
    for (i = 0; i < batch->data_frame_count; ++i) {
        if (batch->fragment_lengths[i] > max_len) {
            max_len = batch->fragment_lengths[i];
        }
    }
    for (p = 0; p < parity_count; ++p) {
        uint8_t parity[XGW_MAX_FRAME_SIZE];
        uint8_t fec_payload[XGW_MAX_FRAME_SIZE];
        xgw_header_t header;
        size_t payload_len;
        size_t b;
        memset(parity, 0, sizeof(parity));
        for (i = 0; i < batch->data_frame_count; ++i) {
            uint8_t coef = matrix[(batch->data_frame_count + p) * batch->data_frame_count + i];
            if (coef == 0U) {
                continue;
            }
            for (b = 0; b < max_len; ++b) {
                uint8_t value = b < batch->fragment_lengths[i] ? batch->fragment_payloads[i][b] : 0U;
                parity[b] ^= gf_mul(coef, value);
            }
        }
        payload_len = xgw_fec_encode(batch->group_id,
                                     (uint8_t) p,
                                     (uint8_t) batch->data_frame_count,
                                     (uint8_t) parity_count,
                                     2U,
                                     parity,
                                     max_len,
                                     fec_payload,
                                     sizeof(fec_payload));
        if (payload_len == 0U) {
            return 0;
        }
        memset(&header, 0, sizeof(header));
        header.version = version;
        header.type = XGW_MESSAGE_FEC;
        header.flags = flags;
        header.session_id = session_id;
        header.stream_id = stream_id;
        header.sequence = seq++;
        header.payload_length = (uint16_t) payload_len;
        if (!xgw_header_encode(&header,
                               fec_payload,
                               payload_len,
                               frames[p],
                               XGW_MAX_FRAME_SIZE,
                               &frame_lengths[p])) {
            return 0;
        }
    }
    *out_count = parity_count;
    *last_sequence = seq - 1U;
    return 1;
}

int xgw_fec_add_data(xgw_fec_codec_t *codec,
                     uint32_t session_id,
                     const char *peer_host,
                     uint16_t peer_port,
                     const uint8_t *payload,
                     size_t payload_len,
                     xgw_reassembly_result_t *result) {
    xgw_fragment_info_t info;
    xgw_fec_group_t *group;
    const uint8_t *fragment_payload = NULL;
    size_t fragment_payload_len = 0U;
    if (codec == NULL) {
        return 0;
    }
    if (!xgw_fragment_decode(payload, payload_len, &info, &fragment_payload, &fragment_payload_len)) {
        return 0;
    }
    if (info.count == 0U || info.index >= info.count || info.count > XGW_MAX_FRAGMENTS) {
        return 0;
    }
    /*
     * 单片 DATA 不需要等待 FEC group。浏览器/HTTPS 回包大多是单片，
     * 如果它先被同 group 的 parity 占位，继续走 group 状态机会造成
     * fragment_buffered，最终让前端等到超时。这里直接快速交付，同时
     * 多片包仍保留 RS/FEC 恢复路径。
     */
    if (info.count == 1U && info.index == 0U) {
        if (fragment_payload_len > sizeof(result->packet)) {
            return 0;
        }
        memset(result, 0, sizeof(*result));
        memcpy(result->packet, fragment_payload, fragment_payload_len);
        result->packet_len = fragment_payload_len;
        remember_completed_group(codec, info.group_id, session_id, peer_host, peer_port);
        return 1;
    }
    if (is_completed_group(codec, info.group_id, session_id, peer_host, peer_port)) {
        return 0;
    }
    group = ensure_group(codec,
                         info.group_id,
                         session_id,
                         peer_host,
                         peer_port,
                         info.count,
                         (uint8_t) (codec->enabled ? codec->parity_shards : 0U));
    if (group == NULL) {
        return 0;
    }
    memcpy(group->fragments[info.index], payload, payload_len);
    group->fragment_lengths[info.index] = payload_len;
    group->fragment_present[info.index] = 1U;
    return try_reassemble(codec, group, result);
}

int xgw_fec_add_parity(xgw_fec_codec_t *codec,
                       uint32_t session_id,
                       const char *peer_host,
                       uint16_t peer_port,
                       const uint8_t *payload,
                       size_t payload_len,
                       xgw_reassembly_result_t *result) {
    xgw_fec_info_t info;
    const uint8_t *parity_payload = NULL;
    size_t parity_payload_len = 0U;
    xgw_fec_group_t *group;
    if (codec == NULL || !codec->enabled) {
        return 0;
    }
    if (!xgw_fec_decode(payload, payload_len, &info, &parity_payload, &parity_payload_len)) {
        return 0;
    }
    if (info.index >= XGW_MAX_FEC_PARITY || info.data_count == 0U) {
        return 0;
    }
    if (is_completed_group(codec, info.group_id, session_id, peer_host, peer_port)) {
        return 0;
    }
    group = ensure_group(codec,
                         info.group_id,
                         session_id,
                         peer_host,
                         peer_port,
                         info.data_count,
                         info.parity_count);
    if (group == NULL) {
        return 0;
    }
    memcpy(group->parity[info.index], parity_payload, parity_payload_len);
    group->parity_lengths[info.index] = parity_payload_len;
    group->parity_present[info.index] = 1U;
    return try_reassemble(codec, group, result);
}

void xgw_fec_reap(xgw_fec_codec_t *codec, time_t now) {
    reap_stale_groups(codec, now);
}
