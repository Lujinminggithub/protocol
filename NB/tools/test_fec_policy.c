#include "../src/nb_fec_policy.h"

#include <stdio.h>
#include <string.h>

static int test_profiles(void){
    nb_fec_profile_t p=nb_fec_profile_select(3.0,40.0,0,0);
    if(p.level!=NB_FEC_PROFILE_BALANCED||p.k!=8||p.r!=1)return -1;
    p=nb_fec_profile_select(5.0,10.0,0,0);
    if(p.level!=NB_FEC_PROFILE_ROBUST||p.k!=8||p.r!=2)return -1;
    p=nb_fec_profile_select(0.0,60.0,0,0);
    if(p.level!=NB_FEC_PROFILE_ROBUST||strcmp(nb_fec_profile_name(p.level),"robust"))return -1;
    p=nb_fec_profile_select(-1.0,-1.0,1,0);
    if(p.level!=NB_FEC_PROFILE_BALANCED)return -1;
    p=nb_fec_profile_select(-1.0,-1.0,1,1);
    return p.level==NB_FEC_PROFILE_ROBUST?0:-1;
}

static int test_start_protocol(void){
    char line[420];nb_fec_start_t start;
    if(nb_fec_start_render(line,sizeof(line),77,4,8,2,"H:next:4443,T:target:443")<0)return -1;
    if(nb_fec_start_parse(line,8,1,&start)!=0||start.session_id!=77||
        start.priority!=4||start.k!=8||start.r!=2||strcmp(start.route,"H:next:4443,T:target:443"))return -1;
    if(nb_fec_start_parse("FC:START:88:4:T:target:443\n",8,1,&start)!=0||
        start.k!=8||start.r!=1||strcmp(start.route,"T:target:443"))return -1;
    if(nb_fec_start_parse("FC:START:0:4:8:1:T:x:1\n",8,1,&start)==0)return -1;
    if(nb_fec_start_parse("FC:START:1:4:9:1:T:x:1\n",8,1,&start)==0)return -1;
    if(nb_fec_start_parse("FC:START:1:4:8:1:\n",8,1,&start)==0)return -1;
    return 0;
}

int main(void){
    if(test_profiles()!=0||test_start_protocol()!=0){fprintf(stderr,"FEC policy tests failed\n");return 1;}
    puts("FEC policy tests passed");return 0;
}
