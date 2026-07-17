#include "nb_control.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int render(const char* command,char* out,size_t cap,void* ctx){
    (void)ctx;if(strcmp(command,"health"))return -1;
    return snprintf(out,cap,"{\"status\":\"ok\"}\n");
}

int main(void){
    char path[96],error[128],response[128];snprintf(path,sizeof(path),"/tmp/nb-control-%ld.sock",(long)getpid());
    int server=nb_control_open(path,error,sizeof(error));if(server<0){puts(error);return 1;}
    struct stat st;if(stat(path,&st)!=0||(st.st_mode&077)!=0)return 1;
    int client=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un addr;memset(&addr,0,sizeof(addr));addr.sun_family=AF_UNIX;strcpy(addr.sun_path,path);
    if(connect(client,(struct sockaddr*)&addr,sizeof(addr))!=0||write(client,"health\n",7)!=7)return 1;
    if(nb_control_serve(server,render,NULL)!=1)return 1;
    ssize_t n=read(client,response,sizeof(response)-1);if(n<0)return 1;response[n]=0;
    close(client);close(server);unlink(path);
    int ok=!strcmp(response,"{\"status\":\"ok\"}\n");printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
