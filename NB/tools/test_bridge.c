#include "nb_bridge.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
    char host[256],rest[300];int port=0;
    assert(nb_bridge_parse_target("H:relay:4443,T:target.example:443",host,sizeof(host),&port)==0);
    assert(!strcmp(host,"target.example")&&port==443);
    assert(nb_bridge_parse_target_exact("T:1.2.3.4:50008",host,sizeof(host),&port)==0&&port==50008);
    assert(nb_bridge_parse_target_exact("H:x:1,T:y:2",host,sizeof(host),&port)!=0);
    assert(nb_bridge_consume_hop("H:relay:4443,T:target:443",host,sizeof(host),&port,rest,sizeof(rest))==0);
    assert(!strcmp(host,"relay")&&port==4443&&!strcmp(rest,"T:target:443"));
    assert(nb_bridge_parse_port("0",&port)!=0&&nb_bridge_parse_port("65536",&port)!=0);
    assert(!strcmp(nb_bridge_role_name(1),"middle"));puts("RESULT PASS");return 0;
}
