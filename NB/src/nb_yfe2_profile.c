#include "nb_yfe2_profile.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

void nb_yfe2_default_profile(nb_yfe2_profile_t* out){
    if(out==NULL)return;
    *out=(nb_yfe2_profile_t){3,16,1,3,4,10,150,1,5000};
}

int nb_yfe2_profile_validate(const nb_yfe2_profile_t* profile){
    nb_yfe2_profile_t expected;nb_yfe2_default_profile(&expected);
    return profile!=NULL&&profile->wire_version==expected.wire_version&&
        profile->data_shards==expected.data_shards&&profile->parity_shards==expected.parity_shards&&
        profile->burst_parity_shards==expected.burst_parity_shards&&profile->interleave==expected.interleave&&
        profile->flush_ms==expected.flush_ms&&profile->recovery_deadline_ms==expected.recovery_deadline_ms&&
        profile->loss_trigger_packets==expected.loss_trigger_packets&&profile->hold_ms==expected.hold_ms?0:-1;
}

int nb_yfe2_profile_render(const nb_yfe2_profile_t* profile,char* out,size_t cap){
    if(nb_yfe2_profile_validate(profile)!=0||out==NULL||cap==0)return -1;
    int length=snprintf(out,cap,
        "codec=nb-yfe2;wire_version=%u;direction=relay_to_exit;data_shards=%u;"
        "parity_shards=%u;burst_parity_shards=%u;interleave=%u;flush_ms=%u;"
        "recovery_deadline_ms=%u;loss_trigger_packets=%llu;hold_ms=%u",
        profile->wire_version,profile->data_shards,profile->parity_shards,
        profile->burst_parity_shards,profile->interleave,profile->flush_ms,
        profile->recovery_deadline_ms,(unsigned long long)profile->loss_trigger_packets,
        profile->hold_ms);
    return length<0||(size_t)length>=cap?-1:length;
}

uint16_t nb_yfe2_profile_id(const nb_yfe2_profile_t* profile){
    char text[NB_YFE2_PROFILE_TEXT_MAX];unsigned char digest[SHA256_DIGEST_LENGTH];
    int length=nb_yfe2_profile_render(profile,text,sizeof(text));
    if(length<0||SHA256((const unsigned char*)text,(size_t)length,digest)==NULL)return 0;
    return (uint16_t)(((uint16_t)digest[0]<<8)|digest[1]);
}
