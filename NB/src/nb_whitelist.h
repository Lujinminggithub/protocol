#ifndef NB_WHITELIST_H
#define NB_WHITELIST_H

#include <stddef.h>

int nb_whitelist_init(const char* path,char* error,size_t error_cap);
int nb_whitelist_allowed(const char* host,int port);
int nb_whitelist_reload_if_changed(char* error,size_t error_cap);
int nb_whitelist_configured(void);

#endif
