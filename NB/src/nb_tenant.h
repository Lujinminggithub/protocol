#ifndef NB_TENANT_H
#define NB_TENANT_H

#include <stddef.h>
#include <stdint.h>

#define NB_TENANT_MAX 64
#define NB_TENANT_NAME_MAX 64
#define NB_TENANT_DEFAULT_BURST_SECONDS 1U
#define NB_TENANT_MAX_BURST_SECONDS 60U
#define NB_TENANT_UP 0
#define NB_TENANT_DOWN 1
#define NB_TENANT_DIRECTIONS 2

typedef struct {
    char name[NB_TENANT_NAME_MAX];
    uint32_t max_tcp;
    uint32_t max_udp;
    uint64_t rate_bytes_per_sec[NB_TENANT_DIRECTIONS];
    uint64_t burst_bytes[NB_TENANT_DIRECTIONS];
    uint64_t byte_quota;
    uint32_t active_tcp;
    uint32_t active_udp;
    uint64_t bytes_up;
    uint64_t bytes_down;
    uint64_t rejected;
    uint64_t tokens[NB_TENANT_DIRECTIONS];
    uint64_t token_updated_us[NB_TENANT_DIRECTIONS];
    uint64_t token_fraction[NB_TENANT_DIRECTIONS];
} nb_tenant_t;

typedef struct {
    nb_tenant_t items[NB_TENANT_MAX];
    size_t count;
    uint64_t config_fingerprint;
    int shared_fd;
    void* shared_state;
} nb_tenants_t;

int nb_tenants_load(nb_tenants_t* out,const char* path,char* error,size_t error_cap);
uint64_t nb_tenants_fingerprint(const nb_tenants_t* tenants);
int nb_tenants_same_identity(const nb_tenants_t* current,const nb_tenants_t* next);
int nb_tenants_reconfigure(nb_tenants_t* current,const nb_tenants_t* next,char* error,size_t error_cap);
int nb_tenants_enable_shared(nb_tenants_t* tenants,const char* path,char* error,size_t error_cap);
void nb_tenants_close(nb_tenants_t* tenants);
int nb_tenant_find(const nb_tenants_t* tenants,const char* name);
int nb_tenant_acquire(nb_tenants_t* tenants,int index,int udp,uint64_t now_us);
void nb_tenant_release(nb_tenants_t* tenants,int index,int udp);
size_t nb_tenant_allowance(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us,int direction);
void nb_tenant_consume(nb_tenants_t* tenants,int index,size_t bytes,int direction);
size_t nb_tenant_take(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us,int direction);
uint64_t nb_tenant_retry_after_us(const nb_tenants_t* tenants,int index,size_t bytes,int direction);
void nb_tenant_refund(nb_tenants_t* tenants,int index,size_t bytes,int direction);
void nb_tenant_refill(uint64_t rate,uint64_t burst,uint64_t now_us,
    uint64_t* tokens,uint64_t* updated_us,uint64_t* fraction);
int64_t nb_tenant_wake_delay(uint64_t throttled_until_us,uint64_t now_us,
    int64_t current_delay_us);
void nb_tenant_account(nb_tenants_t* tenants,int index,uint64_t up,uint64_t down);
int nb_tenants_render_json(const nb_tenants_t* tenants,char* out,size_t cap);

#endif
