#ifndef NB_V2_METADATA_H
#define NB_V2_METADATA_H

#include <stddef.h>
#include <stdint.h>

#define NB_V2_META_MAGIC 0x4e424d32u /* NBM2 */
#define NB_V2_META_VERSION 1u
#define NB_V2_META_TYPE_FLOW_OPEN 1u
#define NB_V2_META_HEADER_SIZE 48u

#define NB_V2_META_FLAG_ALLOW_FEC 0x00000001u
#define NB_V2_META_FLAG_ALLOW_MULTIPATH 0x00000002u
#define NB_V2_META_FLAG_IDEMPOTENT 0x00000004u
#define NB_V2_META_FLAGS_ALL 0x00000007u

#define NB_V2_META_CLASS_RELIABLE 1u
#define NB_V2_META_CLASS_REALTIME 2u
#define NB_V2_META_CLASS_BULK 3u
#define NB_V2_META_CLASS_CONTROL 4u

#define NB_V2_META_PATH_ANY 0u
#define NB_V2_META_PATH_STABLE 1u
#define NB_V2_META_PATH_LOW_JITTER 2u
#define NB_V2_META_PATH_REDUNDANT 3u

#define NB_V2_META_PRIORITY_MAX 7u
#define NB_V2_META_DEADLINE_MAX_MS 60000u
#define NB_V2_META_ROUTE_MAX 300u
#define NB_V2_META_TARGET_MAX 255u
#define NB_V2_META_TAG_MAX 31u

typedef struct {
    uint32_t flags;
    uint8_t traffic_class;
    uint8_t priority;
    uint8_t path_preference;
    uint64_t session_id;
    uint64_t flow_id;
    uint32_t deadline_ms;
    uint32_t policy_id;
    const char* route;
    const char* target;
    const char* business_tag;
} nb_v2_flow_metadata_t;

/* String views point into the decode input and remain valid only while that
 * input buffer remains alive and unchanged. */
typedef struct {
    uint32_t flags;
    uint8_t traffic_class;
    uint8_t priority;
    uint8_t path_preference;
    uint64_t session_id;
    uint64_t flow_id;
    uint32_t deadline_ms;
    uint32_t policy_id;
    const uint8_t* route;
    uint16_t route_length;
    const uint8_t* target;
    uint16_t target_length;
    const uint8_t* business_tag;
    uint16_t business_tag_length;
} nb_v2_flow_metadata_view_t;

int nb_v2_flow_metadata_validate(const nb_v2_flow_metadata_t* metadata);
int nb_v2_flow_metadata_encode(uint8_t* output, size_t capacity,
    const nb_v2_flow_metadata_t* metadata);
int nb_v2_flow_metadata_decode(const uint8_t* data, size_t length,
    nb_v2_flow_metadata_view_t* output);

#endif
