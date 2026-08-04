#include "nb_tenant.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

int main(void){
    assert(nb_tenant_wake_delay(0,1000000,1000000)==1000000);
    assert(nb_tenant_wake_delay(1010000,1000000,1000000)==10000);
    assert(nb_tenant_wake_delay(999999,1000000,1000000)==0);
    assert(nb_tenant_wake_delay(1020000,1000000,5000)==5000);
    char path[96],error[128],json[1024];
#ifndef _WIN32
    char state_path[112];
#endif
#ifdef _WIN32
    snprintf(path,sizeof(path),"nb-tenant-%ld.tmp",(long)getpid());
#else
    snprintf(path,sizeof(path),"/tmp/nb-tenant-%ld",(long)getpid());
#endif
    FILE* f=fopen(path,"w");if(!f)return 1;fputs("tenant alice 1 2 8 1 3\ntenant bob 0 0 16 32 0 2 4\n",f);fclose(f);
    nb_tenants_t tenants;int ok=nb_tenants_load(&tenants,path,error,sizeof(error))==0;
    int alice=nb_tenant_find(&tenants,"alice");ok=ok&&alice==0&&nb_tenant_find(&tenants,"missing")==-1;
    ok=ok&&tenants.items[alice].rate_bytes_per_sec[NB_TENANT_UP]==1000&&tenants.items[alice].rate_bytes_per_sec[NB_TENANT_DOWN]==1000&&tenants.items[alice].burst_bytes[NB_TENANT_UP]==3000&&tenants.items[alice].burst_bytes[NB_TENANT_DOWN]==3000;
    int bob=nb_tenant_find(&tenants,"bob");ok=ok&&bob==1&&tenants.items[bob].rate_bytes_per_sec[NB_TENANT_UP]==2000&&tenants.items[bob].rate_bytes_per_sec[NB_TENANT_DOWN]==4000;
    ok=ok&&nb_tenant_acquire(&tenants,alice,0,1000000)==0&&nb_tenant_acquire(&tenants,alice,0,1000000)!=0;
    nb_tenant_release(&tenants,alice,0);ok=ok&&nb_tenant_acquire(&tenants,alice,0,1000000)==0;
    size_t allowed=nb_tenant_allowance(&tenants,alice,4000,1000000,NB_TENANT_UP);ok=ok&&allowed==3000;nb_tenant_consume(&tenants,alice,3000,NB_TENANT_UP);
    allowed=nb_tenant_allowance(&tenants,alice,1000,1500000,NB_TENANT_UP);ok=ok&&allowed==500;
    ok=ok&&nb_tenant_take(&tenants,bob,4000,1000000,NB_TENANT_UP)==4000&&nb_tenant_allowance(&tenants,bob,1,1000000,NB_TENANT_UP)==0&&nb_tenant_allowance(&tenants,bob,5000,1000000,NB_TENANT_DOWN)==5000;
    nb_tenant_account(&tenants,alice,400,100);ok=ok&&nb_tenants_render_json(&tenants,json,sizeof(json))>0&&strstr(json,"\"bytes_up\":400")&&strstr(json,"\"rate_kbps\":8")&&strstr(json,"\"rate_up_kbps\":8")&&strstr(json,"\"rate_down_kbps\":8");
#ifndef _WIN32
    snprintf(state_path,sizeof(state_path),"%s.state",path);unlink(state_path);
    nb_tenants_t shared;ok=ok&&nb_tenants_load(&shared,path,error,sizeof(error))==0&&
        nb_tenants_enable_shared(&shared,state_path,error,sizeof(error))==0;
    int shared_alice=nb_tenant_find(&shared,"alice");ok=ok&&nb_tenant_acquire(&shared,shared_alice,0,2000000)==0;
    pid_t child=fork();if(child==0){nb_tenants_t peer;if(nb_tenants_load(&peer,path,error,sizeof(error))!=0||
        nb_tenants_enable_shared(&peer,state_path,error,sizeof(error))!=0)_exit(2);
        int rc=nb_tenant_acquire(&peer,0,0,2000000)==0?3:0;nb_tenants_close(&peer);_exit(rc);}
    int status=0;ok=ok&&child>0&&waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
    nb_tenant_release(&shared,shared_alice,0);ok=ok&&nb_tenant_take(&shared,shared_alice,3000,2000000,NB_TENANT_UP)==3000;
    nb_tenant_refund(&shared,shared_alice,250,NB_TENANT_UP);ok=ok&&nb_tenant_allowance(&shared,shared_alice,251,2000000,NB_TENANT_UP)==250&&nb_tenant_allowance(&shared,shared_alice,3000,2000000,NB_TENANT_DOWN)==3000;
    nb_tenants_close(&shared);unlink(state_path);
#endif
    unlink(path);printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
