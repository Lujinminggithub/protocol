#include "nb_whitelist.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void){
    char path[96],error[160];snprintf(path,sizeof(path),"/tmp/nb-whitelist-%ld",(long)getpid());
    FILE* file=fopen(path,"w");if(!file)return 1;
    fputs("domain_suffix tiktok.com\ndomain_exact exact.example\nip 10.0.0.0/8\nport 443\n",file);fclose(file);
    int ok=nb_whitelist_init(path,error,sizeof(error))==0;
    ok=ok&&nb_whitelist_configured()&&nb_whitelist_allowed("api.tiktok.com",443);
    ok=ok&&!nb_whitelist_allowed("nottiktok.com",443)&&!nb_whitelist_allowed("api.tiktok.com",80);
    ok=ok&&nb_whitelist_allowed("exact.example",443)&&!nb_whitelist_allowed("sub.exact.example",443);
    ok=ok&&nb_whitelist_allowed("10.2.3.4",443)&&!nb_whitelist_allowed("11.2.3.4",443);
    sleep(1);file=fopen(path,"w");if(!file)return 1;fputs("unknown value\n",file);fclose(file);
    ok=ok&&nb_whitelist_reload_if_changed(error,sizeof(error))<0;
    ok=ok&&nb_whitelist_allowed("api.tiktok.com",443);
    unlink(path);printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
