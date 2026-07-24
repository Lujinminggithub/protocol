#ifndef NB_TENANT_H
#define NB_TENANT_H

#include <stddef.h>
#include <stdint.h>

#define NB_TENANT_MAX 64
#define NB_TENANT_NAME_MAX 64

typedef struct {
    char name[NB_TENANT_NAME_MAX];
    uint32_t max_tcp;
    uint32_t max_udp;
    uint64_t rate_bytes_per_sec;
    uint64_t byte_quota;
    uint32_t active_tcp;
    uint32_t active_udp;
    uint64_t bytes_up;
    uint64_t bytes_down;
    uint64_t rejected;
    uint64_t tokens;
    uint64_t token_updated_us;
} nb_tenant_t;

typedef struct { nb_tenant_t items[NB_TENANT_MAX]; size_t count; } nb_tenants_t;

int nb_tenants_load(nb_tenants_t* out,const char* path,char* error,size_t error_cap);
int nb_tenant_find(const nb_tenants_t* tenants,const char* name);
int nb_tenant_acquire(nb_tenants_t* tenants,int index,int udp,uint64_t now_us);
void nb_tenant_release(nb_tenants_t* tenants,int index,int udp);
size_t nb_tenant_allowance(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us);
void nb_tenant_consume(nb_tenants_t* tenants,int index,size_t bytes);
void nb_tenant_account(nb_tenants_t* tenants,int index,uint64_t up,uint64_t down);
int nb_tenants_render_json(const nb_tenants_t* tenants,char* out,size_t cap);

#endif
