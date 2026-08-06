#ifndef NB_TRANSPORT_PROFILE_H
#define NB_TRANSPORT_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#define NB_TRANSPORT_CC_MAX 16
#define NB_TRANSPORT_BBR_OPTIONS_MAX 128

typedef struct {
    int present;
    char cc[NB_TRANSPORT_CC_MAX];
    char bbr_options[NB_TRANSPORT_BBR_OPTIONS_MAX];
    uint64_t cwin_max_bytes;
    uint32_t mtu_max;
    uint32_t reorder_gap;
    uint64_t reorder_delay_us;
    int udp_gso;
    int fec_observe;
    int fec_active;
    int udp_fec_adaptive;
    uint32_t udp_fec_k;
    uint64_t udp_fec_hold_us;
    uint64_t target_rate_bps;
    uint64_t seed_rtt_us;
    uint64_t startup_cwin_bytes;
} nb_transport_link_profile_t;

typedef struct {
    uint32_t schema_version;
    uint64_t generation;
    uint64_t fingerprint;
    char line_id[65];
    char role[16];
    nb_transport_link_profile_t ingress;
    nb_transport_link_profile_t egress;
} nb_transport_profile_t;

typedef struct {
    nb_transport_profile_t active;
    nb_transport_profile_t pending;
    nb_transport_profile_t previous;
    int has_active;
    int has_pending;
    int has_previous;
} nb_transport_profile_state_t;

void nb_transport_profile_state_init(nb_transport_profile_state_t* state);
int nb_transport_profile_load(const char* path,nb_transport_profile_t* profile,
    char* error,size_t error_cap);
int nb_transport_profile_prepare(nb_transport_profile_state_t* state,const char* path,
    uint64_t generation,uint64_t fingerprint,const char* line_id,const char* role,
    char* error,size_t error_cap);
int nb_transport_profile_commit(nb_transport_profile_state_t* state,uint64_t generation,
    char* error,size_t error_cap);
int nb_transport_profile_abort(nb_transport_profile_state_t* state,uint64_t generation);
int nb_transport_profile_rollback(nb_transport_profile_state_t* state,uint64_t generation,
    char* error,size_t error_cap);
int nb_transport_profile_render_status(const nb_transport_profile_state_t* state,
    char* out,size_t out_cap);

#endif
