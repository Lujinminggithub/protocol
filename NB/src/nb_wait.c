#include "nb_wait.h"

#include <errno.h>

#define NB_WAIT_MAX_US 1000000LL
#define NB_WAIT_EMPTY_WAKE_BACKOFF_US 1000LL

static int64_t bounded_delay(int64_t delay_us){
    if(delay_us<=0)return 0;
    return delay_us>NB_WAIT_MAX_US?NB_WAIT_MAX_US:delay_us;
}

int nb_wait_timespec(int64_t delay_us,struct timespec* timeout){
    if(timeout==NULL)return -1;
    int64_t bounded=bounded_delay(delay_us);
    timeout->tv_sec=(time_t)(bounded/1000000LL);
    timeout->tv_nsec=(long)((bounded%1000000LL)*1000LL);
    return 0;
}

int nb_wait_fallback_timeout_ms(int64_t delay_us){
    return (int)(bounded_delay(delay_us)/1000LL);
}

int64_t nb_wait_guard_delay(int64_t delay_us,int previous_empty_wake){
    return previous_empty_wake&&delay_us<=0?NB_WAIT_EMPTY_WAKE_BACKOFF_US:delay_us;
}

int nb_wait_empty_wake(int64_t delay_us,int event_count,size_t packets_prepared){
    return delay_us<=0&&event_count==0&&packets_prepared==0;
}

#if defined(__linux__)
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

int nb_epoll_wait_us(int epoll_fd,struct epoll_event* events,int max_events,
    int64_t delay_us){
    struct timespec timeout;
    if(nb_wait_timespec(delay_us,&timeout)!=0){errno=EINVAL;return -1;}
#if defined(SYS_epoll_pwait2)
    int result=(int)syscall(SYS_epoll_pwait2,epoll_fd,events,max_events,
        &timeout,NULL,sizeof(sigset_t));
    if(result>=0||errno!=ENOSYS)return result;
#endif
    return epoll_wait(epoll_fd,events,max_events,
        nb_wait_fallback_timeout_ms(delay_us));
}
#endif
