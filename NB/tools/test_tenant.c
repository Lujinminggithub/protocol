#include "nb_tenant.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void){
    char path[96],error[128],json[1024];
#ifdef _WIN32
    snprintf(path,sizeof(path),"nb-tenant-%ld.tmp",(long)getpid());
#else
    snprintf(path,sizeof(path),"/tmp/nb-tenant-%ld",(long)getpid());
#endif
    FILE* f=fopen(path,"w");if(!f)return 1;fputs("tenant alice 1 2 8 1\ntenant bob 0 0 0 0\n",f);fclose(f);
    nb_tenants_t tenants;int ok=nb_tenants_load(&tenants,path,error,sizeof(error))==0;
    int alice=nb_tenant_find(&tenants,"alice");ok=ok&&alice==0&&nb_tenant_find(&tenants,"missing")==-1;
    ok=ok&&nb_tenant_acquire(&tenants,alice,0,1000000)==0&&nb_tenant_acquire(&tenants,alice,0,1000000)!=0;
    nb_tenant_release(&tenants,alice,0);ok=ok&&nb_tenant_acquire(&tenants,alice,0,1000000)==0;
    size_t allowed=nb_tenant_allowance(&tenants,alice,2000,1000000);ok=ok&&allowed==1000;nb_tenant_consume(&tenants,alice,1000);
    allowed=nb_tenant_allowance(&tenants,alice,1000,1500000);ok=ok&&allowed==500;
    nb_tenant_account(&tenants,alice,400,100);ok=ok&&nb_tenants_render_json(&tenants,json,sizeof(json))>0&&strstr(json,"\"bytes_up\":400");
    unlink(path);printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
