#include "nb_yfe2_profile.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){ \
    fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1; } } while(0)

int main(void){
    static const char expected[]=
        "codec=nb-yfe2;wire_version=3;direction=relay_to_exit;data_shards=16;"
        "parity_shards=1;burst_parity_shards=3;interleave=4;flush_ms=10;"
        "recovery_deadline_ms=150;loss_trigger_packets=1;hold_ms=5000";
    nb_yfe2_profile_t profile;char text[256];
    nb_yfe2_default_profile(&profile);
    CHECK(profile.wire_version==3&&profile.data_shards==16);
    CHECK(profile.parity_shards==1&&profile.burst_parity_shards==3);
    CHECK(profile.interleave==4&&profile.flush_ms==10);
    CHECK(profile.recovery_deadline_ms==150&&profile.loss_trigger_packets==1);
    CHECK(profile.hold_ms==5000&&nb_yfe2_profile_validate(&profile)==0);
    CHECK(nb_yfe2_profile_render(&profile,text,sizeof(text))==(int)strlen(expected));
    CHECK(strcmp(text,expected)==0&&nb_yfe2_profile_id(&profile)==28909);
    profile.interleave=3;CHECK(nb_yfe2_profile_validate(&profile)!=0);
    puts("nb_yfe2_profile_test: ok");return 0;
}
