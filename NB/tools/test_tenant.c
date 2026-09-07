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
    nb_tenants_t retry={0};retry.count=1;retry.items[0].rate_bytes_per_sec[NB_TENANT_UP]=625000;
    assert(nb_tenant_retry_after_us(&retry,0,1,NB_TENANT_UP)==1000);
    assert(nb_tenant_retry_after_us(&retry,0,16384,NB_TENANT_UP)==26215);
    char path[96],reload_path[112],reordered_path[112],explicit_path[112],error[128],json[1024];
#ifndef _WIN32
    char state_path[112];
#endif
#ifdef _WIN32
    snprintf(path,sizeof(path),"nb-tenant-%ld.tmp",(long)getpid());
#else
    snprintf(path,sizeof(path),"/tmp/nb-tenant-%ld",(long)getpid());
#endif
    FILE* f=fopen(path,"w");if(!f)return 1;fputs("tenant alice 1 2 8 1 3\ntenant bob 0 0 16 32 0 2 4\ntenant fast 0 0 5000 0 1\n",f);fclose(f);
    snprintf(reload_path,sizeof(reload_path),"%s.reload",path);
    f=fopen(reload_path,"w");if(!f)return 1;fputs("tenant alice 1 2 80 40 1 1 2\ntenant bob 0 0 16 32 0 2 4\ntenant fast 0 0 5000 0 1\n",f);fclose(f);
    snprintf(reordered_path,sizeof(reordered_path),"%s.reordered",path);
    f=fopen(reordered_path,"w");if(!f)return 1;fputs("tenant bob 0 0 16 32 0 2 4\ntenant alice 1 2 80 40 1 1 2\ntenant fast 0 0 5000 0 1\n",f);fclose(f);
    snprintf(explicit_path,sizeof(explicit_path),"%s.explicit",path);
    f=fopen(explicit_path,"w");if(!f)return 1;fputs("tenant-v3 alice 1 2 5000 8000 0 262144 524288\n",f);fclose(f);
    nb_tenants_t tenants;int ok=nb_tenants_load(&tenants,path,error,sizeof(error))==0;
    int alice=nb_tenant_find(&tenants,"alice");ok=ok&&alice==0&&nb_tenant_find(&tenants,"missing")==-1;
    ok=ok&&tenants.items[alice].rate_bytes_per_sec[NB_TENANT_UP]==1000&&tenants.items[alice].rate_bytes_per_sec[NB_TENANT_DOWN]==1000&&tenants.items[alice].burst_bytes[NB_TENANT_UP]==3000&&tenants.items[alice].burst_bytes[NB_TENANT_DOWN]==3000;
    int bob=nb_tenant_find(&tenants,"bob");ok=ok&&bob==1&&tenants.items[bob].rate_bytes_per_sec[NB_TENANT_UP]==2000&&tenants.items[bob].rate_bytes_per_sec[NB_TENANT_DOWN]==4000;
    nb_tenants_t explicit_limits;ok=ok&&nb_tenants_load(&explicit_limits,explicit_path,error,sizeof(error))==0&&
        explicit_limits.items[0].rate_bytes_per_sec[NB_TENANT_UP]==625000&&
        explicit_limits.items[0].rate_bytes_per_sec[NB_TENANT_DOWN]==1000000&&
        explicit_limits.items[0].burst_bytes[NB_TENANT_UP]==262144&&
        explicit_limits.items[0].burst_bytes[NB_TENANT_DOWN]==524288&&
        explicit_limits.config_fingerprint!=0;
    int fast=nb_tenant_find(&tenants,"fast");ok=ok&&fast==2;
    nb_tenants_t reload,reordered;ok=ok&&nb_tenants_load(&reload,reload_path,error,sizeof(error))==0&&
        nb_tenants_load(&reordered,reordered_path,error,sizeof(error))==0;
    ok=ok&&nb_tenants_same_identity(&tenants,&reload)&&!nb_tenants_same_identity(&tenants,&reordered);
    nb_tenants_t hot=tenants;hot.items[alice].tokens[NB_TENANT_UP]=90000;hot.items[alice].bytes_up=1234;hot.items[alice].active_tcp=1;
    ok=ok&&nb_tenants_reconfigure(&hot,&reload,error,sizeof(error))==0&&
        hot.items[alice].rate_bytes_per_sec[NB_TENANT_UP]==10000&&
        hot.items[alice].rate_bytes_per_sec[NB_TENANT_DOWN]==5000&&
        hot.items[alice].tokens[NB_TENANT_UP]==10000&&hot.items[alice].bytes_up==1234&&hot.items[alice].active_tcp==1;
    ok=ok&&nb_tenants_reconfigure(&hot,&reordered,error,sizeof(error))!=0;
    ok=ok&&nb_tenant_acquire(&tenants,alice,0,1000000)==0&&nb_tenant_acquire(&tenants,alice,0,1000000)!=0;
    nb_tenant_release(&tenants,alice,0);ok=ok&&nb_tenant_acquire(&tenants,alice,0,1000000)==0;
    size_t allowed=nb_tenant_allowance(&tenants,alice,4000,1000000,NB_TENANT_UP);ok=ok&&allowed==3000;nb_tenant_consume(&tenants,alice,3000,NB_TENANT_UP);
    allowed=nb_tenant_allowance(&tenants,alice,1000,1500000,NB_TENANT_UP);ok=ok&&allowed==500;
    ok=ok&&nb_tenant_take(&tenants,bob,4000,1000000,NB_TENANT_UP)==4000&&nb_tenant_allowance(&tenants,bob,1,1000000,NB_TENANT_UP)==0&&nb_tenant_allowance(&tenants,bob,5000,1000000,NB_TENANT_DOWN)==5000;
    ok=ok&&nb_tenant_take(&tenants,fast,625000,1000000,NB_TENANT_UP)==625000;
    size_t local_refilled=0;for(uint64_t now=1000001;now<=1002000;now++)
        local_refilled+=nb_tenant_take(&tenants,fast,1,now,NB_TENANT_UP);
    ok=ok&&local_refilled==1250;
    nb_tenants_t reserve={0};reserve.count=1;reserve.items[0].rate_bytes_per_sec[NB_TENANT_DOWN]=1000;
    reserve.items[0].burst_bytes[NB_TENANT_DOWN]=1000;reserve.items[0].tokens[NB_TENANT_DOWN]=1000;
    reserve.items[0].token_updated_us[NB_TENANT_DOWN]=1000000;
    ok=ok&&nb_tenant_take_class(&reserve,0,100,1000000,NB_TENANT_DOWN,1)==100;
    ok=ok&&nb_tenant_take_class(&reserve,0,1000,1000000,NB_TENANT_DOWN,0)==650;
    ok=ok&&nb_tenant_take_class(&reserve,0,1,1000000,NB_TENANT_DOWN,0)==0;
    ok=ok&&nb_tenant_take_class(&reserve,0,250,1000000,NB_TENANT_DOWN,1)==250;
    ok=ok&&nb_tenant_take_class(&reserve,0,100,1100000,NB_TENANT_DOWN,1)==100;
    ok=ok&&nb_tenant_take_class(&reserve,0,1000,4100001,NB_TENANT_DOWN,0)==1000;
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
    ok=ok&&nb_tenant_take_class(&shared,shared_alice,100,2000000,NB_TENANT_DOWN,1)==100;
    ok=ok&&nb_tenant_take_class(&shared,shared_alice,4000,2000000,NB_TENANT_DOWN,0)==2650;
    ok=ok&&nb_tenant_take_class(&shared,shared_alice,1,2000000,NB_TENANT_DOWN,0)==0;
    ok=ok&&nb_tenant_take_class(&shared,shared_alice,250,2000000,NB_TENANT_DOWN,1)==250;
    int shared_fast=nb_tenant_find(&shared,"fast");ok=ok&&shared_fast==2&&
        nb_tenant_take(&shared,shared_fast,625000,3000000,NB_TENANT_UP)==625000;
    size_t shared_refilled=0;for(uint64_t now=3000001;now<=3002000;now++)
        shared_refilled+=nb_tenant_take(&shared,shared_fast,1,now,NB_TENANT_UP);
    ok=ok&&shared_refilled==1250;
    nb_tenants_close(&shared);unlink(state_path);
#endif
    unlink(explicit_path);unlink(reordered_path);unlink(reload_path);unlink(path);printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
