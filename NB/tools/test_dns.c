#include "nb_dns.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

static struct sockaddr_in test_ipv4(const char *address){
    struct sockaddr_in result;
    result.sin_family=AF_INET;
    result.sin_port=htons(443);
    assert(inet_pton(AF_INET,address,&result.sin_addr)==1);
    return result;
}

int main(void){
    struct sockaddr_in private_address=test_ipv4("10.105.212.98");
    struct sockaddr_in public_address=test_ipv4("203.0.113.10");
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&private_address)==1);
    assert(nb_dns_tiktok_private_answer("rtc-access.tiktokv.com",(const struct sockaddr*)&private_address)==0);
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&public_address)==0);
    assert(nb_dns_tiktok_private_answer("10.105.212.98",(const struct sockaddr*)&private_address)==0);
    assert(nb_dns_servers_valid("1.1.1.1,8.8.8.8")==1);
    assert(nb_dns_servers_valid("1.1.1.1,not-an-ip")==0);
    nb_dns_t* explicit_dns=nb_dns_create_with_servers(AF_INET,1,"1.1.1.1,8.8.8.8");
    assert(explicit_dns!=NULL&&nb_dns_context_submit(explicit_dns,9,"192.0.2.1",443)==0);
    struct pollfd explicit_descriptor={nb_dns_context_result_fd(explicit_dns),POLLIN,0};
    assert(poll(&explicit_descriptor,1,3000)>0);char explicit_drain[8];
    assert(read(explicit_descriptor.fd,explicit_drain,sizeof(explicit_drain))>0);
    nb_dns_result_t explicit_result;assert(nb_dns_context_pop(explicit_dns,&explicit_result)==1&&explicit_result.ok==1);
    assert(strcmp(explicit_result.host,"192.0.2.1")==0);
    assert(explicit_result.completed_at_us>=explicit_result.submitted_at_us);
    nb_dns_destroy(explicit_dns);
    assert(nb_dns_init(AF_UNSPEC,1)==0);assert(nb_dns_result_fd()>=0);
    assert(nb_dns_submit(7,"localhost",443)==0);
    struct pollfd descriptor={nb_dns_result_fd(),POLLIN,0};assert(poll(&descriptor,1,3000)>0);
    char drain[8];ssize_t drained=read(descriptor.fd,drain,sizeof(drain));assert(drained>0);nb_dns_result_t result;
    assert(nb_dns_pop(&result)==1&&result.ps_id==7&&result.port==443&&result.ok==1);
    assert(nb_dns_submit(0,"localhost",443)!=0);puts("RESULT PASS");return 0;
}
