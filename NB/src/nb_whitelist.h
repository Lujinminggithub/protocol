#ifndef NB_WHITELIST_H
#define NB_WHITELIST_H

#include <stddef.h>

typedef struct nb_whitelist nb_whitelist_t;

#define NB_WHITELIST_OPEN_PORT_MIN 50000
#define NB_WHITELIST_OPEN_PORT_MAX 50030

nb_whitelist_t* nb_whitelist_create(const char* path,char* error,size_t error_cap);
void nb_whitelist_destroy(nb_whitelist_t* whitelist);
int nb_whitelist_context_allowed(const nb_whitelist_t* whitelist,const char* host,int port);
int nb_whitelist_context_reload_if_changed(nb_whitelist_t* whitelist,char* error,size_t error_cap);
int nb_whitelist_context_configured(const nb_whitelist_t* whitelist);

int nb_whitelist_init(const char* path,char* error,size_t error_cap);
int nb_whitelist_allowed(const char* host,int port);
int nb_whitelist_reload_if_changed(char* error,size_t error_cap);
int nb_whitelist_configured(void);

#endif
