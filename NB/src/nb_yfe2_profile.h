#ifndef NB_YFE2_PROFILE_H
#define NB_YFE2_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#define NB_YFE2_PROFILE_ID 28909u
#define NB_YFE2_PROFILE_TEXT_MAX 256u

typedef struct {
    uint8_t wire_version;
    uint8_t data_shards;
    uint8_t parity_shards;
    uint8_t burst_parity_shards;
    uint8_t interleave;
    uint16_t flush_ms;
    uint16_t recovery_deadline_ms;
    uint64_t loss_trigger_packets;
    uint16_t hold_ms;
} nb_yfe2_profile_t;

void nb_yfe2_default_profile(nb_yfe2_profile_t* out);
int nb_yfe2_profile_validate(const nb_yfe2_profile_t* profile);
int nb_yfe2_profile_render(const nb_yfe2_profile_t* profile,char* out,size_t cap);
uint16_t nb_yfe2_profile_id(const nb_yfe2_profile_t* profile);

#endif
