#ifndef NB_TENANT_SHARED_H
#define NB_TENANT_SHARED_H

#include "nb_tenant.h"

int nb_tenant_shared_open(nb_tenants_t* tenants,const char* path,char* error,size_t error_cap);
void nb_tenant_shared_close(nb_tenants_t* tenants);
int nb_tenant_shared_acquire(nb_tenants_t* tenants,int index,int udp,uint64_t now_us);
void nb_tenant_shared_release(nb_tenants_t* tenants,int index,int udp);
size_t nb_tenant_shared_allowance(nb_tenants_t* tenants,int index,size_t requested,uint64_t now_us,int take,int direction);
void nb_tenant_shared_refund(nb_tenants_t* tenants,int index,size_t bytes,int direction);
void nb_tenant_shared_account(nb_tenants_t* tenants,int index,uint64_t up,uint64_t down);
int nb_tenant_shared_snapshot(nb_tenants_t* tenants);
int nb_tenant_shared_reconfigure(nb_tenants_t* tenants,const nb_tenants_t* next,char* error,size_t error_cap);

#endif
