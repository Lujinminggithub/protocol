#ifndef NB_EXIT_NODE_FACADE_H
#define NB_EXIT_NODE_FACADE_H

#include "nb_bridge.h"
#include "nb_exit_recovery.h"
#include "nb_metrics.h"

#define NB_EXIT_TARGET_ROLE_CLOSE_REASON "target-role-invalid"

static inline nb_exit_recovery_action_t nb_exit_node_target_route(
    nb_exit_recovery_state_t* state, nb_role_t role)
{
    if(state == NULL || !state->active || state->terminal_claimed)
        return NB_EXIT_RECOVERY_NONE;
    if(role == ROLE_EXIT)return NB_EXIT_RECOVERY_TARGET_ALLOWED;
    state->dns_pending = 0;
    state->tcp_connecting = 0;
    state->target_connect_state = 3;
    state->terminal_claimed = 1;
    return NB_EXIT_RECOVERY_ROLE_REJECT;
}

static inline void nb_exit_node_note_dns(nb_metrics_state_t* metrics, nb_role_t role,
    int failed, int private_rejected, uint64_t latency_us)
{
    if(role == ROLE_EXIT)
        nb_metrics_note_dns(metrics, failed, private_rejected, latency_us);
}

static inline void nb_exit_node_note_target_connect(nb_metrics_state_t* metrics,
    nb_role_t role, int timeout, uint64_t latency_us)
{
    if(role == ROLE_EXIT)
        nb_metrics_note_target_connect(metrics, timeout, latency_us);
}

#endif
