#include "nb_udp_io.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

static int set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    return flags<0?-1:fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}

int nb_udp_bind_relay_socket(uint16_t port_min,uint16_t port_max,
    uint16_t* next_port,struct sockaddr_in* bound){
    if(next_port==NULL||bound==NULL||((port_min==0)!=(port_max==0))||
        (port_min!=0&&port_min>port_max)){errno=EINVAL;return -1;}
    uint32_t attempts=port_min?(uint32_t)port_max-port_min+1u:1u;
    uint16_t start=(*next_port>=port_min&&*next_port<=port_max)?*next_port:port_min;
    for(uint32_t i=0;i<attempts;i++){
        int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
        struct sockaddr_in address;memset(&address,0,sizeof(address));address.sin_family=AF_INET;
        address.sin_addr.s_addr=INADDR_ANY;
        if(port_min){uint32_t offset=((uint32_t)start-port_min+i)%attempts;
            address.sin_port=htons((uint16_t)(port_min+offset));}
        if(set_nonblocking(fd)==0&&bind(fd,(struct sockaddr*)&address,sizeof(address))==0){
            socklen_t address_length=sizeof(address);
            if(getsockname(fd,(struct sockaddr*)&address,&address_length)!=0){int saved=errno;close(fd);errno=saved;return -1;}
            if(port_min){uint16_t port=ntohs(address.sin_port);
                *next_port=port==port_max?port_min:(uint16_t)(port+1);}
            *bound=address;return fd;
        }
        int saved=errno;close(fd);errno=saved;
        if(!port_min||errno!=EADDRINUSE)return -1;
    }
    errno=EADDRINUSE;return -1;
}

int nb_udp_endpoint_equal(const struct sockaddr_storage* a,
    const struct sockaddr_storage* b){
    if(a==NULL||b==NULL||a->ss_family!=b->ss_family)return 0;
    if(a->ss_family==AF_INET){const struct sockaddr_in* x=(const void*)a,*y=(const void*)b;
        return x->sin_port==y->sin_port&&x->sin_addr.s_addr==y->sin_addr.s_addr;}
    if(a->ss_family==AF_INET6){const struct sockaddr_in6* x=(const void*)a,*y=(const void*)b;
        return x->sin6_port==y->sin6_port&&memcmp(&x->sin6_addr,&y->sin6_addr,16)==0;}
    return 0;
}

size_t nb_udp_send_gso(int fd,const uint8_t* buffer,size_t length,
    size_t segment_size,const struct sockaddr* destination,
    socklen_t destination_length){
    if(fd<0||buffer==NULL||length==0||segment_size==0||segment_size>UINT16_MAX||
        destination==NULL||destination_length==0){errno=EINVAL;return 1;}
    if(length<=segment_size)return sendto(fd,buffer,length,0,destination,destination_length)<0;
    struct msghdr message;memset(&message,0,sizeof(message));
    struct iovec iov={(void*)buffer,length};message.msg_name=(void*)destination;
    message.msg_namelen=destination_length;message.msg_iov=&iov;message.msg_iovlen=1;
    char control[CMSG_SPACE(sizeof(uint16_t))];memset(control,0,sizeof(control));
    message.msg_control=control;message.msg_controllen=sizeof(control);
    struct cmsghdr* header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_UDP;
    header->cmsg_type=UDP_SEGMENT;header->cmsg_len=CMSG_LEN(sizeof(uint16_t));
    *((uint16_t*)CMSG_DATA(header))=(uint16_t)segment_size;
    if(sendmsg(fd,&message,0)>=0)return 0;
    size_t errors=0;
    for(size_t offset=0;offset<length;offset+=segment_size){
        size_t current=length-offset<segment_size?length-offset:segment_size;
        errors+=(sendto(fd,buffer+offset,current,0,destination,destination_length)<0);
    }
    return errors;
}
