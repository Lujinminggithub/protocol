#ifndef NB_EXIT_RECOVERY_H
#define NB_EXIT_RECOVERY_H

#include <limits.h>
#include <stdint.h>

#include "nb_connect.h"

typedef enum {
    NB_EXIT_RECOVERY_NONE = 0,
    NB_EXIT_RECOVERY_START_CONNECT,
    NB_EXIT_RECOVERY_DNS_FAIL,
    NB_EXIT_RECOVERY_PRIVATE_REJECT,
    NB_EXIT_RECOVERY_CONNECT_OK,
    NB_EXIT_RECOVERY_CONNECT_FAIL,
    NB_EXIT_RECOVERY_CONNECT_TIMEOUT
} nb_exit_recovery_action_t;

/* Exit 目标解析/建连阶段的可测试状态视图。terminal_claimed 保证失败动作只消费一次。 */
typedef struct {
    int active;
    int dns_pending;
    int tcp_connecting;
    int target_connect_state;
    int terminal_claimed;
    uint64_t target_connect_at_us;
    uint64_t target_connect_done_at_us;
} nb_exit_recovery_state_t;

static inline nb_exit_recovery_action_t nb_exit_recovery_dns_result(
    nb_exit_recovery_state_t* state, int ok, int private_rejected, uint64_t now_us)
{
    if(state == NULL || !state->active || !state->dns_pending || state->terminal_claimed)
        return NB_EXIT_RECOVERY_NONE;
    state->dns_pending = 0;
    if(!ok || private_rejected){
        state->target_connect_state = 3;
        state->target_connect_done_at_us = now_us;
        state->terminal_claimed = 1;
        return !ok ? NB_EXIT_RECOVERY_DNS_FAIL : NB_EXIT_RECOVERY_PRIVATE_REJECT;
    }
    return NB_EXIT_RECOVERY_START_CONNECT;
}

static inline void nb_exit_recovery_connect_started(nb_exit_recovery_state_t* state,
    uint64_t now_us)
{
    if(state == NULL || !state->active || state->terminal_claimed)return;
    state->tcp_connecting = 1;
    state->target_connect_state = 1;
    state->target_connect_at_us = now_us;
    state->target_connect_done_at_us = 0;
}

static inline nb_exit_recovery_action_t nb_exit_recovery_connect_complete(
    nb_exit_recovery_state_t* state, int ok, uint64_t now_us)
{
    if(state == NULL || !state->active || !state->tcp_connecting || state->terminal_claimed)
        return NB_EXIT_RECOVERY_NONE;
    state->tcp_connecting = 0;
    state->target_connect_done_at_us = now_us;
    state->target_connect_state = ok ? 2 : 3;
    if(ok)return NB_EXIT_RECOVERY_CONNECT_OK;
    state->terminal_claimed = 1;
    return NB_EXIT_RECOVERY_CONNECT_FAIL;
}

static inline nb_exit_recovery_action_t nb_exit_recovery_connect_timeout(
    nb_exit_recovery_state_t* state, uint64_t now_us)
{
    if(state == NULL || !state->active || !state->tcp_connecting || state->terminal_claimed ||
        !nb_connect_expired(state->target_connect_at_us, now_us))
        return NB_EXIT_RECOVERY_NONE;
    state->tcp_connecting = 0;
    state->target_connect_state = 3;
    state->target_connect_done_at_us = now_us;
    state->terminal_claimed = 1;
    return NB_EXIT_RECOVERY_CONNECT_TIMEOUT;
}

static inline int64_t nb_exit_recovery_wait_us(const nb_exit_recovery_state_t* state,
    uint64_t now_us, int64_t current_wait_us)
{
    if(state == NULL || !state->active || !state->tcp_connecting ||
        state->terminal_claimed || current_wait_us <= 0)
        return current_wait_us;
    uint64_t deadline_us = nb_connect_deadline_us(state->target_connect_at_us);
    if(deadline_us <= now_us)return 0;
    uint64_t remaining_us = deadline_us - now_us;
    if(remaining_us > (uint64_t)INT64_MAX)return current_wait_us;
    return (int64_t)remaining_us < current_wait_us ? (int64_t)remaining_us : current_wait_us;
}

static inline uint64_t nb_exit_recovery_latency_us(const nb_exit_recovery_state_t* state)
{
    if(state == NULL || state->target_connect_at_us == 0 ||
        state->target_connect_done_at_us < state->target_connect_at_us)
        return 0;
    return state->target_connect_done_at_us - state->target_connect_at_us;
}

#endif
