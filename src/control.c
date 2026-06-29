/* 控制面状态机：HELLO/HELLO_ACK/CONFIRM/KEEPALIVE。 */

#include "xgw_control.h"

#include <string.h>

static void zero_result(xgw_control_result_t *result) {
    memset(result, 0, sizeof(*result));
}

void xgw_control_make_hello(xgw_security_material_t *sec,
                            const xgw_negotiation_info_t *negotiation,
                            uint32_t mtu_offer,
                            xgw_hello_payload_t *hello) {
    uint8_t auth_input[32];
    memset(hello, 0, sizeof(*hello));
    hello->features = (uint32_t) negotiation->congestion_mode | ((uint32_t) negotiation->bbr_profile << 8U);
    hello->mtu_offer = mtu_offer;
    hello->rx_bps = negotiation->local_rx_bps;
    hello->tx_bps = negotiation->peer_rx_bps;
    hello->nonce = sec->local_nonce;
    memcpy(auth_input, hello, 32U);
    xgw_security_auth_tag(sec, auth_input, 32U, hello->auth_tag);
}

void xgw_control_make_hello_ack(xgw_security_material_t *sec,
                                const xgw_negotiation_info_t *negotiation,
                                const xgw_hello_payload_t *hello,
                                uint32_t mtu_accept,
                                xgw_hello_ack_payload_t *hello_ack) {
    uint8_t auth_input[40];
    memset(hello_ack, 0, sizeof(*hello_ack));
    hello_ack->features = (uint32_t) negotiation->congestion_mode | ((uint32_t) negotiation->bbr_profile << 8U);
    hello_ack->mtu_accept = mtu_accept;
    hello_ack->rx_bps = negotiation->local_rx_bps;
    hello_ack->tx_bps = negotiation->peer_rx_bps;
    hello_ack->hello_nonce = hello->nonce;
    hello_ack->ack_nonce = sec->local_nonce;
    memcpy(auth_input, hello_ack, 40U);
    xgw_security_auth_tag(sec, auth_input, 40U, hello_ack->auth_tag);
}

void xgw_control_make_confirm(xgw_security_material_t *sec,
                              const xgw_hello_ack_payload_t *hello_ack,
                              xgw_confirm_payload_t *confirm) {
    uint8_t auth_input[16];
    memset(confirm, 0, sizeof(*confirm));
    confirm->hello_nonce = hello_ack->hello_nonce;
    confirm->ack_nonce = hello_ack->ack_nonce;
    memcpy(auth_input, confirm, 16U);
    xgw_security_auth_tag(sec, auth_input, 16U, confirm->auth_tag);
}

void xgw_control_make_keepalive(const xgw_ack_info_t *ack,
                                uint64_t timestamp_us,
                                xgw_keepalive_payload_t *keepalive) {
    memset(keepalive, 0, sizeof(*keepalive));
    keepalive->timestamp_us = timestamp_us;
    if (ack != NULL) {
        keepalive->ack = *ack;
    }
}

void xgw_control_make_ack(const xgw_ack_info_t *ack,
                          uint64_t timestamp_us,
                          xgw_keepalive_payload_t *feedback) {
    xgw_control_make_keepalive(ack, timestamp_us, feedback);
}

int xgw_control_process(uint8_t type,
                        const uint8_t *payload,
                        size_t payload_len,
                        xgw_security_material_t *sec,
                        xgw_control_state_t *state,
                        const xgw_negotiation_info_t *negotiation,
                        uint32_t mtu,
                        xgw_control_result_t *result) {
    uint32_t prev_state;
    zero_result(result);
    result->handled = 1;
    prev_state = (uint32_t) (*state);
    switch (type) {
        case XGW_MESSAGE_HELLO: {
            xgw_hello_payload_t hello;
            xgw_hello_ack_payload_t hello_ack;
            uint8_t auth_input[32];
            if (!xgw_decode_hello_payload(payload, payload_len, &hello)) {
                return 0;
            }
            memcpy(auth_input, &hello, 32U);
            if (!xgw_security_verify_auth_tag(sec, auth_input, 32U, hello.auth_tag)) {
                return 0;
            }
            if (*state == XGW_CTRL_ESTABLISHED) {
                xgw_control_make_hello_ack(sec,
                                           negotiation,
                                           &hello,
                                           mtu < hello.mtu_offer ? mtu : hello.mtu_offer,
                                           &hello_ack);
                result->emit_response = 1;
                result->response_type = XGW_MESSAGE_HELLO_ACK;
                result->response_flags = XGW_FLAG_CONTROL;
                result->response_payload_len = xgw_encode_hello_ack_payload(&hello_ack,
                                                                            result->response_payload,
                                                                            sizeof(result->response_payload));
                xgw_security_complete_handshake(sec, sec->local_nonce, hello.nonce, 0);
                result->established = 1;
                result->reset_transport_state = 1;
                return result->response_payload_len > 0U;
            }
            *state = XGW_CTRL_HELLO_RCVD;
            xgw_control_make_hello_ack(sec, negotiation, &hello, mtu < hello.mtu_offer ? mtu : hello.mtu_offer, &hello_ack);
            result->emit_response = 1;
            result->response_type = XGW_MESSAGE_HELLO_ACK;
            result->response_flags = XGW_FLAG_CONTROL;
            result->response_payload_len = xgw_encode_hello_ack_payload(&hello_ack,
                                                                        result->response_payload,
                                                                        sizeof(result->response_payload));
            /* 对称同时握手（双方都先发 HELLO）下，若两端都按固定 responder 派生密钥，
             * tx/rx 方向会相同 → 密钥不配对 → 后续数据帧解密全失败、session 永远 startup。
             * 用 nonce 大小做确定性 tie-break 决定 initiator 角色：两端比较同一对 nonce
             * 必得相反结论（一 initiator 一 responder），密钥派生方向配对。64bit 随机 nonce
             * 碰撞可忽略。非对称握手在 HELLO_ACK 分支收敛，不经此处，故不受影响。 */
            xgw_security_complete_handshake(sec, sec->local_nonce, hello.nonce,
                                            sec->local_nonce > hello.nonce);
            result->reset_transport_state = 1;
            result->established = (*state == XGW_CTRL_ESTABLISHED);
            return result->response_payload_len > 0U;
        }
        case XGW_MESSAGE_HELLO_ACK: {
            xgw_hello_ack_payload_t hello_ack;
            xgw_confirm_payload_t confirm;
            uint8_t auth_input[40];
            if (*state == XGW_CTRL_ESTABLISHED) {
                result->established = 1;
                return 1;
            }
            if (*state != XGW_CTRL_HELLO_SENT && *state != XGW_CTRL_HELLO_RCVD) {
                return 1;
            }
            if (!xgw_decode_hello_ack_payload(payload, payload_len, &hello_ack)) {
                return 0;
            }
            memcpy(auth_input, &hello_ack, 40U);
            if (!xgw_security_verify_auth_tag(sec, auth_input, 40U, hello_ack.auth_tag)) {
                return 0;
            }
            if (prev_state == XGW_CTRL_HELLO_SENT) {
                /* 非对称：本端是发起方，对端先以 responder 回了 HELLO_ACK。
                 * 维持原有 initiator 语义（local=本端 hello_nonce, peer=ack_nonce）。 */
                xgw_security_complete_handshake(sec,
                                                hello_ack.hello_nonce,
                                                hello_ack.ack_nonce,
                                                1);
            } else {
                /* 对称：双方都先发过 HELLO，密钥已在 HELLO 分支按 nonce tie-break 派生。
                 * 此处用同一对 nonce、同一 tie-break 规则重算（幂等），确保 CONFIRM 阶段
                 * 与 HELLO 分支角色一致，避免又退回固定 responder 覆盖掉正确密钥。 */
                xgw_security_complete_handshake(sec,
                                                sec->local_nonce,
                                                hello_ack.ack_nonce,
                                                sec->local_nonce > hello_ack.ack_nonce);
            }
            xgw_control_make_confirm(sec, &hello_ack, &confirm);
            if (prev_state == XGW_CTRL_HELLO_SENT) {
                *state = XGW_CTRL_ESTABLISHED;
                result->became_established = 1;
            } else {
                *state = XGW_CTRL_CONFIRM_SENT;
            }
            result->established = 1;
            result->reset_transport_state = 1;
            result->emit_response = 1;
            result->response_type = XGW_MESSAGE_CONFIRM;
            result->response_flags = XGW_FLAG_CONTROL;
            result->response_payload_len = xgw_encode_confirm_payload(&confirm,
                                                                      result->response_payload,
                                                                      sizeof(result->response_payload));
            return result->response_payload_len > 0U;
        }
        case XGW_MESSAGE_CONFIRM: {
            xgw_confirm_payload_t confirm;
            uint8_t auth_input[16];
            if (!xgw_decode_confirm_payload(payload, payload_len, &confirm)) {
                return 0;
            }
            memcpy(auth_input, &confirm, 16U);
            if (!xgw_security_verify_auth_tag(sec, auth_input, 16U, confirm.auth_tag)) {
                return 0;
            }
            *state = XGW_CTRL_ESTABLISHED;
            result->established = 1;
            result->became_established = 1;
            result->reset_transport_state = 1;
            return 1;
        }
        case XGW_MESSAGE_KEEPALIVE: {
            xgw_keepalive_payload_t keepalive;
            if (!xgw_decode_keepalive_payload(payload, payload_len, &keepalive)) {
                return 0;
            }
            result->ack = keepalive.ack;
            result->peer_timestamp_us = keepalive.timestamp_us;
            result->established = (*state == XGW_CTRL_ESTABLISHED);
            return 1;
        }
        case XGW_MESSAGE_ACK: {
            xgw_keepalive_payload_t keepalive;
            if (!xgw_decode_keepalive_payload(payload, payload_len, &keepalive)) {
                return 0;
            }
            result->ack = keepalive.ack;
            result->peer_timestamp_us = keepalive.timestamp_us;
            result->established = (*state == XGW_CTRL_ESTABLISHED);
            return 1;
        }
        default:
            result->handled = 0;
            return 1;
    }
}
