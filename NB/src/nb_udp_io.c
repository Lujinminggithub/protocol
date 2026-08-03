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
#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

static int set_nonblocking(int fd){
    int flags=fcntl(fd,F_GETFL,0);
    return flags<0?-1:fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}

int nb_udp_enable_rxq_overflow(int fd){
    if(fd<0){errno=EINVAL;return -1;}
    int enabled=1;
    return setsockopt(fd,SOL_SOCKET,SO_RXQ_OVFL,&enabled,sizeof(enabled));
}

int nb_udp_configure_buffers(int fd,int requested,
    int* actual_receive,int* actual_send){
    if(fd<0||requested<=0){errno=EINVAL;return -1;}
    if(setsockopt(fd,SOL_SOCKET,SO_RCVBUFFORCE,&requested,sizeof(requested))<0&&
        setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&requested,sizeof(requested))<0)return -1;
    if(setsockopt(fd,SOL_SOCKET,SO_SNDBUFFORCE,&requested,sizeof(requested))<0&&
        setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&requested,sizeof(requested))<0)return -1;
    int receive=0,send=0;socklen_t length=sizeof(receive);
    if(getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&receive,&length)<0)return -1;
    length=sizeof(send);
    if(getsockopt(fd,SOL_SOCKET,SO_SNDBUF,&send,&length)<0)return -1;
    if(actual_receive)*actual_receive=receive;
    if(actual_send)*actual_send=send;
    return 0;
}

uint64_t nb_udp_rxq_overflow_update(nb_udp_rxq_state_t* state,
    const struct msghdr* message){
    if(state==NULL||message==NULL)return 0;
    for(struct cmsghdr* header=CMSG_FIRSTHDR(message);header!=NULL;
        header=CMSG_NXTHDR((struct msghdr*)message,header)){
        if(header->cmsg_level!=SOL_SOCKET||header->cmsg_type!=SO_RXQ_OVFL||
            header->cmsg_len<CMSG_LEN(sizeof(uint32_t)))continue;
        uint32_t observed=0;memcpy(&observed,CMSG_DATA(header),sizeof(observed));
        uint64_t delta=state->seen?(uint32_t)(observed-state->last):observed;
        state->last=observed;state->seen=1;return delta;
    }
    return 0;
}

ssize_t nb_udp_recv(int fd,void* buffer,size_t length,int flags,
    struct sockaddr* source,socklen_t* source_length,
    nb_udp_rxq_state_t* rxq_state,uint64_t* dropped){
    if(dropped)*dropped=0;
    if(fd<0||buffer==NULL||length==0||(source!=NULL&&source_length==NULL)){
        errno=EINVAL;return -1;
    }
    struct iovec iov={buffer,length};struct msghdr message;memset(&message,0,sizeof(message));
    message.msg_iov=&iov;message.msg_iovlen=1;message.msg_name=source;
    message.msg_namelen=source_length?*source_length:0;
    union { struct cmsghdr align;uint8_t bytes[CMSG_SPACE(sizeof(uint32_t))]; } control;
    memset(&control,0,sizeof(control));message.msg_control=control.bytes;
    message.msg_controllen=sizeof(control.bytes);
    ssize_t received=recvmsg(fd,&message,flags);
    if(received>=0){
        if(source_length)*source_length=message.msg_namelen;
        if(dropped)*dropped=nb_udp_rxq_overflow_update(rxq_state,&message);
    }
    return received;
}

int nb_udp_bind_relay_socket(uint16_t port_min,uint16_t port_max,
    uint16_t* next_port,struct sockaddr_in* bound){
    if(next_port==NULL||bound==NULL||((port_min==0)!=(port_max==0))||
        (port_min!=0&&port_min>port_max)){errno=EINVAL;return -1;}
    uint32_t attempts=port_min?(uint32_t)port_max-port_min+1u:1u;
    uint16_t start=(*next_port>=port_min&&*next_port<=port_max)?*next_port:port_min;
    for(uint32_t i=0;i<attempts;i++){
        int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
        if(nb_udp_enable_rxq_overflow(fd)!=0){int saved=errno;close(fd);errno=saved;return -1;}
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
