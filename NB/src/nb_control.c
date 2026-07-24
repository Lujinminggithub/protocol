#include "nb_control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int nonblock(int fd){int flags=fcntl(fd,F_GETFL,0);return flags<0?-1:fcntl(fd,F_SETFL,flags|O_NONBLOCK);}

int nb_control_open(const char* path,char* error,size_t error_cap){
    if(path==NULL||path[0]==0||strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path)){
        if(error&&error_cap)snprintf(error,error_cap,"invalid control socket path");
        return -1;
    }
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0){if(error&&error_cap)snprintf(error,error_cap,"socket: %s",strerror(errno));return -1;}
    struct sockaddr_un addr;memset(&addr,0,sizeof(addr));addr.sun_family=AF_UNIX;memcpy(addr.sun_path,path,strlen(path)+1);
    unlink(path);
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))!=0||chmod(path,0600)!=0||listen(fd,16)!=0||nonblock(fd)!=0){
        if(error&&error_cap)snprintf(error,error_cap,"bind/listen %s: %s",path,strerror(errno));
        close(fd);unlink(path);return -1;
    }
    return fd;
}

int nb_control_serve(int listen_fd,nb_control_render_fn render,void* ctx){
    int served=0;
    for(;;){
        int fd=accept4(listen_fd,NULL,NULL,SOCK_CLOEXEC);
        if(fd<0){if(errno==EAGAIN||errno==EWOULDBLOCK)return served;return -1;}
        struct timeval timeout={.tv_sec=0,.tv_usec=200000};
        (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        char request[128],response[8192];ssize_t n=read(fd,request,sizeof(request)-1);
        if(n>0){
            request[n]=0;request[strcspn(request,"\r\n")]=0;
            int len=render?render(request,response,sizeof(response),ctx):-1;
            if(len<0){len=snprintf(response,sizeof(response),"{\"error\":\"unknown command\"}\n");}
            if((size_t)len>=sizeof(response))len=(int)sizeof(response)-1;
            size_t off=0;
            while(off<(size_t)len){
                ssize_t written=write(fd,response+off,(size_t)len-off);
                if(written>0)off+=(size_t)written;else break;
            }
        }
        close(fd);served++;
    }
}
