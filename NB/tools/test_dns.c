#include "nb_dns.h"
#include "nb_exit_recovery.h"
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
    struct sockaddr_in private_172_address=test_ipv4("172.20.1.2");
    struct sockaddr_in private_192_address=test_ipv4("192.168.10.2");
    struct sockaddr_in public_address=test_ipv4("203.0.113.10");
    struct sockaddr_in outside_172_low=test_ipv4("172.15.255.255");
    struct sockaddr_in outside_172_high=test_ipv4("172.32.0.1");
    struct sockaddr_in outside_192_low=test_ipv4("192.167.255.255");
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&private_address)==1);
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&private_172_address)==1);
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&private_192_address)==1);
    assert(nb_dns_tiktok_private_answer("FOO.SG-FN.TIKTOK-ROW.NET",(const struct sockaddr*)&private_address)==1);
    assert(nb_dns_tiktok_private_answer("rtc-access.tiktokv.com",(const struct sockaddr*)&private_address)==0);
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&public_address)==0);
    assert(nb_dns_tiktok_private_answer("10.105.212.98",(const struct sockaddr*)&private_address)==0);
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&outside_172_low)==0);
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&outside_172_high)==0);
    assert(nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&outside_192_low)==0);
    nb_exit_recovery_state_t private_result={.active=1,.dns_pending=1};
    assert(nb_exit_recovery_dns_result(&private_result,1,
        nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&private_address),1234ULL)==
        NB_EXIT_RECOVERY_PRIVATE_REJECT);
    assert(private_result.dns_pending==0&&private_result.target_connect_state==3);
    assert(private_result.terminal_claimed==1&&private_result.target_connect_done_at_us==1234ULL);
    assert(nb_exit_recovery_dns_result(&private_result,1,1,2000ULL)==NB_EXIT_RECOVERY_NONE);
    nb_exit_recovery_state_t public_result={.active=1,.dns_pending=1};
    assert(nb_exit_recovery_dns_result(&public_result,1,
        nb_dns_tiktok_private_answer("foo.sg-fn.tiktok-row.net",(const struct sockaddr*)&public_address),1300ULL)==
        NB_EXIT_RECOVERY_START_CONNECT);
    assert(public_result.dns_pending==0&&public_result.terminal_claimed==0);
    nb_exit_recovery_connect_started(&public_result,1500ULL);
    assert(public_result.tcp_connecting==1&&public_result.target_connect_state==1);
    assert(public_result.target_connect_at_us==1500ULL);
    nb_exit_recovery_state_t failed_result={.active=1,.dns_pending=1};
    assert(nb_exit_recovery_dns_result(&failed_result,0,0,1400ULL)==NB_EXIT_RECOVERY_DNS_FAIL);
    assert(failed_result.target_connect_state==3&&failed_result.terminal_claimed==1);
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
