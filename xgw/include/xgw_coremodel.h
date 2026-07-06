#ifndef XGW_COREMODEL_H
#define XGW_COREMODEL_H

#include <stdint.h>

#define XGW_FLOW_PRIORITY_BULK 0U
#define XGW_FLOW_PRIORITY_DEFAULT 1U
#define XGW_FLOW_PRIORITY_INTERACTIVE 2U
#define XGW_FLOW_PRIORITY_CRITICAL 3U

#define XGW_FLOW_BUDGET_REDUCED_FEC 0x01U
#define XGW_FLOW_BUDGET_FAST_ACK 0x02U
#define XGW_FLOW_BUDGET_INDEPENDENT_IO 0x04U

typedef struct xgw_flow_hint {
    uint8_t valid;
    uint8_t flow_class;
    uint8_t priority;
    uint8_t budget_flags;
    uint8_t preferred_copies;
    char frontend[16];
    char session_id[40];
    char route_name[24];
    char line_id[16];
    uint32_t read_timeout_ms;
    uint32_t idle_after_first_byte_ms;
    uint32_t runtime_inflight_bytes;
    uint32_t runtime_send_credit_bytes;
    uint16_t runtime_return_delay_ms;
    uint8_t runtime_ack_credit_frames;
    uint32_t runtime_deficit_bytes;
    uint16_t runtime_starvation_boost;
    uint8_t runtime_parity_budget;
    uint16_t runtime_feedback_cadence_ms;
    uint32_t runtime_loss_ppm;
    uint32_t runtime_recovered_ppm;
} xgw_flow_hint_t;

#endif
