#ifndef XGW_CONTROL_H
#define XGW_CONTROL_H

/* 控制面状态机：握手、确认、keepalive 与 ACK 摘要。 */

#include "xgw_protocol.h"
#include "xgw_security.h"

#include <stddef.h>
#include <stdint.h>

typedef struct xgw_control_result {
    int handled;
    int established;
    int reset_transport_state;
    int became_established;
    int emit_response;
    uint8_t response_type;
    uint8_t response_payload[256];
    size_t response_payload_len;
    uint16_t response_flags;
    xgw_ack_info_t ack;
    uint64_t peer_timestamp_us;
} xgw_control_result_t;

void xgw_control_make_hello(xgw_security_material_t *sec,
                            const xgw_negotiation_info_t *negotiation,
                            uint32_t mtu_offer,
                            xgw_hello_payload_t *hello);
void xgw_control_make_hello_ack(xgw_security_material_t *sec,
                                const xgw_negotiation_info_t *negotiation,
                                const xgw_hello_payload_t *hello,
                                uint32_t mtu_accept,
                                xgw_hello_ack_payload_t *hello_ack);
void xgw_control_make_confirm(xgw_security_material_t *sec,
                              const xgw_hello_ack_payload_t *hello_ack,
                              xgw_confirm_payload_t *confirm);
void xgw_control_make_keepalive(const xgw_ack_info_t *ack,
                                uint64_t timestamp_us,
                                xgw_keepalive_payload_t *keepalive);
void xgw_control_make_ack(const xgw_ack_info_t *ack,
                          uint64_t timestamp_us,
                          xgw_keepalive_payload_t *feedback);

int xgw_control_process(uint8_t type,
                        const uint8_t *payload,
                        size_t payload_len,
                        xgw_security_material_t *sec,
                        xgw_control_state_t *state,
                        const xgw_negotiation_info_t *negotiation,
                        uint32_t mtu,
                        xgw_control_result_t *result);

#endif
