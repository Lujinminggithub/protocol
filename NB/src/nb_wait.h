#ifndef NB_WAIT_H
#define NB_WAIT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

int nb_wait_timespec(int64_t delay_us,struct timespec* timeout);
int nb_wait_fallback_timeout_ms(int64_t delay_us);
int64_t nb_wait_guard_delay(int64_t delay_us,int previous_empty_wake);
int nb_wait_empty_wake(int64_t delay_us,int event_count,size_t packets_prepared);

#if defined(__linux__)
#include <sys/epoll.h>
int nb_epoll_wait_us(int epoll_fd,struct epoll_event* events,int max_events,
    int64_t delay_us);
#endif

#endif
