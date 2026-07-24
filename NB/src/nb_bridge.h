#ifndef NB_BRIDGE_H
#define NB_BRIDGE_H

#include <stddef.h>

typedef enum { ROLE_ENTRY = 0, ROLE_MIDDLE = 1, ROLE_EXIT = 2 } nb_role_t;

const char* nb_bridge_role_name(int role);
int nb_bridge_parse_port(const char* text,int* port);
int nb_bridge_parse_target(const char* route,char* host,size_t host_cap,int* port);
int nb_bridge_parse_target_exact(const char* route,char* host,size_t host_cap,int* port);
int nb_bridge_consume_hop(const char* route,char* next_host,size_t host_cap,int* next_port,
    char* rest,size_t rest_cap);

#endif
