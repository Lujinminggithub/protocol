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

typedef struct nb_dns nb_dns_t;

nb_dns_t* nb_dns_create(int address_family,int worker_count);
nb_dns_t* nb_dns_create_with_servers(int address_family,int worker_count,const char* servers);
int nb_dns_servers_valid(const char* servers);
void nb_dns_destroy(nb_dns_t* dns);
int nb_dns_context_result_fd(nb_dns_t* dns);
int nb_dns_context_submit(nb_dns_t* dns,uint32_t session_id,const char* host,int port);
int nb_dns_context_pop(nb_dns_t* dns,nb_dns_result_t* result);

int nb_dns_init(int address_family,int worker_count);
int nb_dns_result_fd(void);
int nb_dns_submit(uint32_t session_id,const char* host,int port);
int nb_dns_pop(nb_dns_result_t* result);

#endif
