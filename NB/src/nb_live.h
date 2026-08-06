#ifndef NB_LIVE_H
#define NB_LIVE_H

#include <stddef.h>
#include <stdint.h>

#include "nb_policy.h"

typedef struct {
    size_t high_bytes;
    size_t low_bytes;
    uint64_t deadline_us;
} nb_live_queue_limits_t;

typedef struct {
    uint64_t queued_at;
    size_t bytes;
} nb_live_queue_segment_t;

#define NB_LIVE_QUEUE_SEGMENTS 128

typedef struct {
    nb_live_queue_segment_t segments[NB_LIVE_QUEUE_SEGMENTS];
    size_t head;
    size_t count;
} nb_live_queue_clock_t;

typedef struct {
    uint64_t sampled_at;
    uint64_t sampled_c2s;
    uint64_t sampled_s2c;
    unsigned high_uplink_windows;
    unsigned high_ctrl_windows;
    unsigned high_downlink_windows;
} nb_live_flow_runtime_t;

typedef enum {
    NB_LIVE_FLOW_KEEP = 0,
    NB_LIVE_FLOW_PROMOTE_MEDIA = 1,
    NB_LIVE_FLOW_DEMOTE_BULK = 2
} nb_live_flow_action_t;

typedef struct {
    uint64_t updated_at;
    double ctrl_tokens;
} nb_live_sched_t;

nb_live_queue_limits_t nb_live_queue_limits(nb_flow_class_t flow_class, int udp_mode);
nb_live_queue_limits_t nb_live_queue_limits_for_path(nb_flow_class_t flow_class, int udp_mode,
    uint64_t reorder_delay_us);
int nb_live_queue_expiry_enabled(nb_flow_class_t flow_class, int udp_mode);
void nb_live_queue_appended(nb_live_queue_clock_t* clock, size_t previous_len, size_t added, uint64_t now_us);
void nb_live_queue_consumed(nb_live_queue_clock_t* clock, size_t consumed, size_t remaining);
uint64_t nb_live_queue_age_us(const nb_live_queue_clock_t* clock, size_t length, uint64_t now_us);
nb_live_flow_action_t nb_live_flow_observe(nb_live_flow_runtime_t* runtime,
    nb_flow_class_t flow_class, uint64_t total_c2s, uint64_t total_s2c, uint64_t now_us,
    double* c2s_kbps, double* s2c_kbps);
size_t nb_live_sched_grant(nb_live_sched_t* sched, nb_flow_class_t flow_class,
    size_t requested, uint64_t now_us, int media_pending);
int nb_live_sched_ctrl_ready(nb_live_sched_t* sched, uint64_t now_us);

#endif
