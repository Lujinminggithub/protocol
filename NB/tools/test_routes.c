#include "nb_routes.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void){
    char path[96],error[128];snprintf(path,sizeof(path),"/tmp/nb-routes-%ld",(long)getpid());
    FILE* f=fopen(path,"w");if(!f)return 1;fputs("route kz1 H:10.0.0.1:4443 2\nroute kz2 H:10.0.0.2:4443 1\n",f);fclose(f);
    nb_routes_t routes;int ok=nb_routes_load(&routes,path,error,sizeof(error))==0;
    const char* expected[]={"kz1","kz1","kz2","kz1"};
    for(int i=0;i<4&&ok;i++){const nb_route_entry_t* route=nb_routes_pick(&routes);ok=route&&!strcmp(route->name,expected[i]);}
    unlink(path);printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
