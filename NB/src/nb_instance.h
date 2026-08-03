#ifndef NB_INSTANCE_H
#define NB_INSTANCE_H

#include <stdint.h>
#include <stdatomic.h>
#include <netinet/in.h>

#include "nb_auth.h"
#include "nb_auth_async.h"
#include "nb_dns.h"
#include "nb_fec.h"
#include "nb_live.h"
#include "nb_metrics.h"
#include "nb_pool_health.h"
#include "nb_probe.h"
#include "nb_runtime.h"
#include "nb_session_index.h"
#include "nb_tenant.h"
#include "nb_transport_profile.h"
#include "nb_udp_io.h"
#include "nb_udp_lifecycle.h"
#include "nb_whitelist.h"
#include "nb_policy.h"

#define NB_FEC_SESSION_INDEX_CAP 2048u
#define NB_SCHED_CNX_CAP 64u
#define NB_INSTANCE_DEFAULT_QUEUE_LIMIT (64ULL*1024ULL*1024ULL)
#define NB_INSTANCE_ENV_MAX 48

typedef struct { uint32_t sid; proxy_stream_t* stream; } nb_fec_session_index_t;
typedef struct {
    picoquic_cnx_t* cnx;
    nb_live_sched_t sched;
    uint64_t transport_generation;
    int transport_profile_bound;
} nb_sched_cnx_t;

typedef struct {
    nb_transport_link_profile_t link;
    uint64_t generation;
    int active;
} nb_transport_runtime_t;

typedef struct nb_instance {
    nb_global_t global;
    uint64_t run_id;
    nb_auth_users_t socks_users;
    nb_auth_async_t* auth_async;
    nb_dns_t* dns;
    nb_whitelist_t* whitelist;
    nb_policy_t* policy;
    int socks_auth_enabled;
    nb_tenants_t tenants;
    int tenants_enabled;
    struct in_addr socks_udp_advertise_addr;
    int socks_udp_advertise_configured;
    uint16_t socks_udp_port_min;
    uint16_t socks_udp_port_max;
    uint16_t socks_udp_port_next;
    int streams_inuse;
    int streams_peak;
    int udp_gso_enabled;
    uint64_t reorder_gap;
    uint64_t reorder_delay_us;
    nb_pool_health_config_t pool_health_config;
    int fec_v15_observe;
    int fec_v15_active;
    int fec_v15_force;
    int fec_v15_force_robust;
    uint32_t fec_v15_drop_src_mod;
    uint32_t fec_v15_drop_repair_mod;
    uint64_t fec_v15_drop_src;
    uint64_t fec_v15_drop_repair;
    nb_fec_config_t fec_config;
    uint64_t fec_bdp_bitrate_bps;
    uint64_t fec_bdp_rtt_us;
    unsigned fec_bdp_multiplier;
    nb_fec_metrics_t fec_metrics_done;
    nb_metrics_state_t metrics_state;
    nb_udp_rxq_state_t udp_rxq_main;
    nb_udp_rxq_state_t udp_rxq_client;
    nb_session_index_t session_index;
    nb_udp_lifecycle_t udp_lifecycle;
    uint64_t udp_control_grace_us;
    uint64_t memory_released_pending;
    nb_fec_session_index_t fec_session_index[NB_FEC_SESSION_INDEX_CAP];
    nb_sched_cnx_t sched_cnx[NB_SCHED_CNX_CAP];
    uint64_t queue_bytes;
    uint64_t queue_limit_bytes;
    uint32_t session_limit;
    uint64_t pool_health_last_at;
    uint32_t datagram_rr;
    uint64_t rxq_last_warn_at;
    uint64_t udp_send_last_warn_at;
    uint64_t memory_scan_at;
    uint64_t malloc_trim_at;
    uint64_t stream_usage_last_at;
    uint64_t whitelist_last_at;
    uint64_t link_quality_last_at;
    uint64_t fec_stats_last_at;
    nb_transport_profile_state_t transport_profiles;
    nb_transport_runtime_t transport_ingress;
    nb_transport_runtime_t transport_egress;
    struct {char name[64];char value[512];} environment[NB_INSTANCE_ENV_MAX];
    size_t environment_count;
    atomic_int stop_requested;
    atomic_int ready;
    atomic_int running;
    int managed;
    int exit_code;
    char instance_id[65];
    char control_path[108];
} nb_instance_t;

void nb_instance_init(nb_instance_t* instance);
const char* nb_instance_env(const nb_instance_t* instance,const char* name);
int nb_instance_set_env(nb_instance_t* instance,const char* name,const char* value);

#endif
