#include "nb_dns.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int main(void){
    assert(nb_dns_init(AF_UNSPEC,1)==0);assert(nb_dns_result_fd()>=0);
    assert(nb_dns_submit(7,"localhost",443)==0);
    struct pollfd descriptor={nb_dns_result_fd(),POLLIN,0};assert(poll(&descriptor,1,3000)>0);
    char drain[8];ssize_t drained=read(descriptor.fd,drain,sizeof(drain));assert(drained>0);nb_dns_result_t result;
    assert(nb_dns_pop(&result)==1&&result.ps_id==7&&result.port==443&&result.ok==1);
    assert(nb_dns_submit(0,"localhost",443)!=0);puts("RESULT PASS");return 0;
}
