#include "nb_udp_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

int main(void){
    struct sockaddr_in bound;uint16_t next=0;
    int sender=nb_udp_bind_relay_socket(0,0,&next,&bound);CHECK(sender>=0);
    CHECK((fcntl(sender,F_GETFL,0)&O_NONBLOCK)!=0);
    int overflow_enabled=0;socklen_t option_length=sizeof(overflow_enabled);
    CHECK(getsockopt(sender,SOL_SOCKET,SO_RXQ_OVFL,&overflow_enabled,&option_length)==0);
    CHECK(overflow_enabled==1);
    int receive_buffer=0,send_buffer=0;
    CHECK(nb_udp_configure_buffers(sender,256*1024,&receive_buffer,&send_buffer)==0);
    CHECK(receive_buffer>0);
    CHECK(send_buffer>0);
    errno=0;
    CHECK(nb_udp_configure_buffers(-1,256*1024,NULL,NULL)<0&&errno==EINVAL);

    int receiver=socket(AF_INET,SOCK_DGRAM,0);CHECK(receiver>=0);
    struct sockaddr_in destination={0};destination.sin_family=AF_INET;
    destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);destination.sin_port=0;
    CHECK(bind(receiver,(struct sockaddr*)&destination,sizeof(destination))==0);
    socklen_t destination_length=sizeof(destination);
    CHECK(getsockname(receiver,(struct sockaddr*)&destination,&destination_length)==0);

    const uint8_t payload[]="udp-io";
    CHECK(nb_udp_send_gso(sender,payload,sizeof(payload),sizeof(payload),
        (struct sockaddr*)&destination,destination_length)==0);
    CHECK(nb_udp_enable_rxq_overflow(receiver)==0);
    uint8_t received[32]={0};nb_udp_rxq_state_t receive_state={0};uint64_t dropped=99;
    CHECK(nb_udp_recv(receiver,received,sizeof(received),0,NULL,NULL,&receive_state,&dropped)==(ssize_t)sizeof(payload));
    CHECK(dropped==0);
    CHECK(memcmp(received,payload,sizeof(payload))==0);

    union { struct cmsghdr align;uint8_t bytes[CMSG_SPACE(sizeof(uint32_t))]; } control={0};
    struct msghdr message={0};message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    struct cmsghdr* header=CMSG_FIRSTHDR(&message);CHECK(header!=NULL);
    header->cmsg_level=SOL_SOCKET;header->cmsg_type=SO_RXQ_OVFL;
    header->cmsg_len=CMSG_LEN(sizeof(uint32_t));uint32_t observed=7;
    memcpy(CMSG_DATA(header),&observed,sizeof(observed));nb_udp_rxq_state_t state={0};
    CHECK(nb_udp_rxq_overflow_update(&state,&message)==7);
    observed=11;memcpy(CMSG_DATA(header),&observed,sizeof(observed));
    CHECK(nb_udp_rxq_overflow_update(&state,&message)==4);
    state.last=UINT32_MAX-1u;state.seen=1;observed=1;
    memcpy(CMSG_DATA(header),&observed,sizeof(observed));
    CHECK(nb_udp_rxq_overflow_update(&state,&message)==3);

    struct sockaddr_storage a={0},b={0};
    memcpy(&a,&destination,sizeof(destination));memcpy(&b,&destination,sizeof(destination));
    CHECK(nb_udp_endpoint_equal(&a,&b));
    ((struct sockaddr_in*)&b)->sin_port=htons((uint16_t)(ntohs(destination.sin_port)+1));
    CHECK(!nb_udp_endpoint_equal(&a,&b));
    errno=0;CHECK(nb_udp_bind_relay_socket(20000,0,&next,&bound)<0&&errno==EINVAL);

    close(receiver);close(sender);puts("nb_udp_io_test: ok");return 0;
}
