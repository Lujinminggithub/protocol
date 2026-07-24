#ifndef NB_DNS_H
#define NB_DNS_H

#include <stdint.h>
#include <sys/socket.h>

typedef struct {
    uint32_t ps_id;
    int port;
    int ok;
    struct sockaddr_storage addr;
    socklen_t addrlen;
} nb_dns_result_t;

int nb_dns_init(int address_family,int worker_count);
int nb_dns_result_fd(void);
int nb_dns_submit(uint32_t session_id,const char* host,int port);
int nb_dns_pop(nb_dns_result_t* result);

#endif
