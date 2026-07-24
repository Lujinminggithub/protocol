#include "nb_routes.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void){
    char path[96],error[128];
#ifdef _WIN32
    snprintf(path,sizeof(path),"nb-routes-%ld.tmp",(long)getpid());
#else
    snprintf(path,sizeof(path),"/tmp/nb-routes-%ld",(long)getpid());
#endif
    FILE* f=fopen(path,"w");if(!f)return 1;fputs("route kz1 H:10.0.0.1:4443 2 1\nroute kz2 H:10.0.0.2:4443 1 2\n",f);fclose(f);
    nb_routes_t routes;int ok=nb_routes_load(&routes,path,error,sizeof(error))==0;
    const char* expected[]={"kz1","kz1","kz2","kz1"};
    for(int i=0;i<4&&ok;i++){const nb_route_entry_t* route=nb_routes_pick(&routes);ok=route&&!strcmp(route->name,expected[i]);}
    uint64_t key=nb_routes_hash("alice","live",443);const nb_route_entry_t* sticky=nb_routes_pick_key(&routes,key,1000);ok=ok&&sticky==nb_routes_pick_key(&routes,key,1000);
    int index=nb_routes_acquire(&routes,&routes.entries[0]);ok=ok&&index==0&&routes.entries[index].active==1;
    ok=ok&&nb_routes_acquire(&routes,&routes.entries[0])<0;nb_routes_release(&routes,(size_t)index);
    index=nb_routes_acquire(&routes,&routes.entries[0]);routes.entries[index].failures=2;
    nb_routes_finish_session(&routes,(size_t)index);
    ok=ok&&routes.entries[index].active==0&&routes.entries[index].failures==2&&routes.entries[index].unhealthy_until_us==0;
    for(int i=0;i<3;i++)nb_routes_report(&routes,(size_t)index,0,2000,30000);
    ok=ok&&routes.entries[index].unhealthy_until_us==32000;
    nb_routes_report(&routes,(size_t)index,1,33000,30000);ok=ok&&routes.entries[index].unhealthy_until_us==0;
    nb_routes_report_result(&routes,(size_t)index,0,34000,30000);ok=ok&&routes.entries[index].failures==0;
    ok=ok&&nb_routes_report_name(&routes,"kz1",-1,35000,30000)==0&&routes.entries[index].failures==1;
    ok=ok&&nb_routes_report_name(&routes,"missing",-1,35000,30000)<0;
    ok=ok&&nb_routes_report_name(&routes,"kz1",1,36000,30000)==0&&routes.entries[index].failures==0;
    char response[160];ok=ok&&nb_routes_control_command(&routes,"route-result kz1 failure",response,sizeof(response),37000,30000)>0;
    ok=ok&&strstr(response,"\"status\":\"ok\"")!=NULL&&routes.entries[index].failures==1;
    ok=ok&&nb_routes_control_command(&routes,"route-result kz1 invalid",response,sizeof(response),38000,30000)>0;
    ok=ok&&strstr(response,"\"error\"")!=NULL;
    unlink(path);printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
