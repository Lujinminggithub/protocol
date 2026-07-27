#ifndef NB_FEC_POLICY_H
#define NB_FEC_POLICY_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    NB_FEC_PROFILE_BALANCED = 1,
    NB_FEC_PROFILE_ROBUST = 2
} nb_fec_profile_level_t;

typedef struct {
    nb_fec_profile_level_t level;
    uint8_t k;
    uint8_t r;
} nb_fec_profile_t;

typedef struct {
    uint32_t session_id;
    int priority;
    uint8_t k;
    uint8_t r;
    char route[300];
} nb_fec_start_t;

nb_fec_profile_t nb_fec_profile_select(double loss_pct, double jitter_ms,
    int coldstart, int robust_override);
const char* nb_fec_profile_name(nb_fec_profile_level_t level);

int nb_fec_start_render(char* out, size_t cap, uint32_t session_id, int priority,
    uint8_t k, uint8_t r, const char* route);
int nb_fec_start_parse(const char* line, uint8_t legacy_k, uint8_t legacy_r,
    nb_fec_start_t* out);

#endif
