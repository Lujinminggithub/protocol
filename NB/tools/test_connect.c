#include <assert.h>
#include <stdint.h>

#include "nb_connect.h"
#include "nb_exit_recovery.h"

int main(void)
{
    assert(nb_connect_deadline_us(UINT64_MAX) == UINT64_MAX);
    assert(nb_connect_expired(UINT64_MAX, UINT64_MAX) == 0);
    assert(nb_connect_expired(0, 5000000ULL) == 0);
    assert(nb_connect_expired(1000000ULL, 5999999ULL) == 0);
    assert(nb_connect_expired(1000000ULL, 6000000ULL) == 1);
    assert(nb_connect_deadline_us(UINT64_MAX - 4999999ULL) == UINT64_MAX);
    assert(nb_connect_deadline_us(1000000ULL) == 6000000ULL);

    nb_exit_recovery_state_t pending = {
        .active = 1,
        .dns_pending = 0,
        .tcp_connecting = 1,
        .target_connect_state = 1,
        .target_connect_at_us = 1000000ULL
    };
    assert(nb_exit_recovery_wait_us(&pending, 2000000ULL, 1000000) == 1000000);
    assert(nb_exit_recovery_wait_us(&pending, 5999999ULL, 1000000) == 1);
    assert(nb_exit_recovery_wait_us(&pending, 6000000ULL, 1000000) == 0);
    assert(nb_exit_recovery_connect_timeout(&pending, 5999999ULL) == NB_EXIT_RECOVERY_NONE);
    assert(nb_exit_recovery_connect_timeout(&pending, 6000000ULL) == NB_EXIT_RECOVERY_CONNECT_TIMEOUT);
    assert(pending.tcp_connecting == 0);
    assert(pending.target_connect_state == 3);
    assert(pending.target_connect_done_at_us == 6000000ULL);
    assert(nb_exit_recovery_latency_us(&pending) == 5000000ULL);
    assert(nb_exit_recovery_connect_timeout(&pending, 7000000ULL) == NB_EXIT_RECOVERY_NONE);

    nb_exit_recovery_state_t connected = {
        .active = 1,
        .tcp_connecting = 1,
        .target_connect_state = 1,
        .target_connect_at_us = 1000000ULL
    };
    assert(nb_exit_recovery_connect_complete(&connected, 1, 1123456ULL) == NB_EXIT_RECOVERY_CONNECT_OK);
    assert(connected.tcp_connecting == 0);
    assert(connected.target_connect_state == 2);
    assert(nb_exit_recovery_latency_us(&connected) == 123456ULL);

    nb_exit_recovery_state_t failed = {
        .active = 1,
        .tcp_connecting = 1,
        .target_connect_state = 1,
        .target_connect_at_us = 2000000ULL
    };
    assert(nb_exit_recovery_connect_complete(&failed, 0, 2100000ULL) == NB_EXIT_RECOVERY_CONNECT_FAIL);
    assert(failed.terminal_claimed == 1);
    assert(nb_exit_recovery_connect_complete(&failed, 0, 2200000ULL) == NB_EXIT_RECOVERY_NONE);
    return 0;
}
