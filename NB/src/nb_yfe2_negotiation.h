#ifndef NB_YFE2_NEGOTIATION_H
#define NB_YFE2_NEGOTIATION_H

#include <stddef.h>
#include <stdint.h>

#include "nb_yfe2_profile.h"

#define NB_YFE2_CONTROL_SIZE 40u
#define NB_YFE2_NEGOTIATION_TIMEOUT_US 500000ULL

enum { NB_YFE2_CTL_PROPOSE=1,NB_YFE2_CTL_ACCEPT=2,NB_YFE2_CTL_REJECT=3 };
enum { NB_YFE2_REASON_NONE=0,NB_YFE2_REASON_NOT_SUPPORTED=1,
    NB_YFE2_REASON_PROFILE_MISMATCH=2,NB_YFE2_REASON_PMTU=3,
    NB_YFE2_REASON_RESOURCE=4 };

typedef struct {
    uint8_t type;
    uint8_t reason;
    uint64_t nonce;
    uint16_t profile_id;
    nb_yfe2_profile_t profile;
} nb_yfe2_control_t;

typedef enum {
    NB_YFE2_NEG_DISABLED=0,NB_YFE2_NEG_PROPOSED,NB_YFE2_NEG_ACCEPTED,
    NB_YFE2_NEG_FALLBACK_TIMEOUT,NB_YFE2_NEG_FALLBACK_NOT_SUPPORTED,
    NB_YFE2_NEG_FALLBACK_PROFILE_MISMATCH,NB_YFE2_NEG_FALLBACK_PMTU,
    NB_YFE2_NEG_FALLBACK_RESOURCE
} nb_yfe2_negotiation_state_t;

typedef struct {
    nb_yfe2_negotiation_state_t state;
    uint64_t nonce;
    uint64_t deadline_us;
    uint16_t profile_id;
} nb_yfe2_negotiation_t;

int nb_yfe2_control_encode(uint8_t* out,size_t cap,const nb_yfe2_control_t* control);
int nb_yfe2_control_decode(const uint8_t* wire,size_t length,nb_yfe2_control_t* out);
void nb_yfe2_negotiation_init(nb_yfe2_negotiation_t* state);
int nb_yfe2_negotiation_start(nb_yfe2_negotiation_t* state,uint64_t nonce,uint64_t now_us);
nb_yfe2_negotiation_state_t nb_yfe2_negotiation_timeout(nb_yfe2_negotiation_t* state,uint64_t now_us);
int nb_yfe2_negotiation_accept(nb_yfe2_negotiation_t* state,uint64_t nonce,uint16_t profile_id);
int nb_yfe2_negotiation_reject(nb_yfe2_negotiation_t* state,uint64_t nonce,uint8_t reason);
const char* nb_yfe2_negotiation_state_name(nb_yfe2_negotiation_state_t state);

#endif
