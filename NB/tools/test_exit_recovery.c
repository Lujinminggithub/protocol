#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "nb_exit_node_facade.h"

static void assert_non_exit_rejected(nb_role_t role)
{
    nb_exit_recovery_state_t state = {.active = 1};
    nb_metrics_state_t metrics = {0};

    assert(nb_exit_node_target_route(&state, role) == NB_EXIT_RECOVERY_ROLE_REJECT);
    assert(state.terminal_claimed == 1);
    assert(state.target_connect_state == 3);
    assert(strcmp(NB_EXIT_TARGET_ROLE_CLOSE_REASON, "target-role-invalid") == 0);

    nb_exit_node_note_dns(&metrics, role, 1, 1, 1200);
    nb_exit_node_note_target_connect(&metrics, role, 1, 5000000);
    assert(metrics.dns_requests == 0);
    assert(metrics.dns_failures == 0);
    assert(metrics.dns_private_rejected == 0);
    assert(metrics.dns_latency_max_us == 0);
    assert(metrics.target_connect_timeouts == 0);
    assert(metrics.target_connect_latency_max_us == 0);
}

int main(void)
{
    assert_non_exit_rejected(ROLE_ENTRY);
    assert_non_exit_rejected(ROLE_MIDDLE);

    nb_exit_recovery_state_t state = {.active = 1};
    nb_metrics_state_t metrics = {0};
    assert(nb_exit_node_target_route(&state, ROLE_EXIT) == NB_EXIT_RECOVERY_TARGET_ALLOWED);
    assert(state.terminal_claimed == 0);
    nb_exit_node_note_dns(&metrics, ROLE_EXIT, 0, 0, 1200);
    nb_exit_node_note_target_connect(&metrics, ROLE_EXIT, 0, 90000);
    assert(metrics.dns_requests == 1);
    assert(metrics.dns_latency_max_us == 1200);
    assert(metrics.target_connect_latency_max_us == 90000);
    return 0;
}
