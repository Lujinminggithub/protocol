#include "nb_udp_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)

int main(void){
    struct sockaddr_in bound;uint16_t next=0;
    int sender=nb_udp_bind_relay_socket(0,0,&next,&bound);CHECK(sender>=0);
    CHECK((fcntl(sender,F_GETFL,0)&O_NONBLOCK)!=0);

    int receiver=socket(AF_INET,SOCK_DGRAM,0);CHECK(receiver>=0);
    struct sockaddr_in destination={0};destination.sin_family=AF_INET;
    destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);destination.sin_port=0;
    CHECK(bind(receiver,(struct sockaddr*)&destination,sizeof(destination))==0);
    socklen_t destination_length=sizeof(destination);
    CHECK(getsockname(receiver,(struct sockaddr*)&destination,&destination_length)==0);

    const uint8_t payload[]="udp-io";
    CHECK(nb_udp_send_gso(sender,payload,sizeof(payload),sizeof(payload),
        (struct sockaddr*)&destination,destination_length)==0);
    uint8_t received[32]={0};CHECK(recv(receiver,received,sizeof(received),0)==(ssize_t)sizeof(payload));
    CHECK(memcmp(received,payload,sizeof(payload))==0);

    struct sockaddr_storage a={0},b={0};
    memcpy(&a,&destination,sizeof(destination));memcpy(&b,&destination,sizeof(destination));
    CHECK(nb_udp_endpoint_equal(&a,&b));
    ((struct sockaddr_in*)&b)->sin_port=htons((uint16_t)(ntohs(destination.sin_port)+1));
    CHECK(!nb_udp_endpoint_equal(&a,&b));
    errno=0;CHECK(nb_udp_bind_relay_socket(20000,0,&next,&bound)<0&&errno==EINVAL);

    close(receiver);close(sender);puts("nb_udp_io_test: ok");return 0;
}
