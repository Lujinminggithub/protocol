#ifndef NB_TENANT_WIRE_H
#define NB_TENANT_WIRE_H

#include <stddef.h>

#define NB_TENANT_WIRE_PREFIX "A="

int nb_tenant_wire_name_valid(const char* name);
int nb_tenant_wire_render(char* out,size_t cap,const char* tenant,const char* route);
int nb_tenant_wire_parse(const char* input,char* tenant,size_t tenant_cap,const char** route);

#endif
