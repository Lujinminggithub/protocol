#ifndef NB_WAIT_H
#define NB_WAIT_H

#include <stdint.h>
#include <time.h>

int nb_wait_timespec(int64_t delay_us,struct timespec* timeout);
int nb_wait_fallback_timeout_ms(int64_t delay_us);

#if defined(__linux__)
#include <sys/epoll.h>
int nb_epoll_wait_us(int epoll_fd,struct epoll_event* events,int max_events,
    int64_t delay_us);
#endif

#endif
