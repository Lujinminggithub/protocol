#ifndef NB_POLICY_H
#define NB_POLICY_H

#include <stddef.h>

#define NB_PRIO_MEDIA 4
#define NB_PRIO_CTRL 2
#define NB_PRIO_BULK 20

typedef enum {
    NB_FLOW_CLASS_UNKNOWN = 0,
    NB_FLOW_CLASS_CTRL = 1,
    NB_FLOW_CLASS_MEDIA = 2,
    NB_FLOW_CLASS_BULK = 3
} nb_flow_class_t;

typedef enum {
    NB_FLOW_LANE_DEFAULT = 0,
    NB_FLOW_LANE_LATENCY = 1,
    NB_FLOW_LANE_BULK = 2
} nb_flow_lane_t;

typedef enum {
    NB_FLOW_FEC_DEFAULT = 0,
    NB_FLOW_FEC_AUTO = 1,
    NB_FLOW_FEC_OFF = 2,
    NB_FLOW_FEC_FORCE_ON = 3
} nb_flow_fec_t;

typedef struct {
    int matched;
    nb_flow_class_t flow_class;
    nb_flow_lane_t lane_hint;
    nb_flow_fec_t fec_hint;
    int prio;
    char rule_name[64];
} nb_flow_policy_t;

typedef struct nb_policy nb_policy_t;

nb_policy_t* nb_policy_create(const char* rules_path);
void nb_policy_destroy(nb_policy_t* policy);
int nb_policy_reload_if_changed(nb_policy_t* policy);
int nb_policy_context_classify(nb_policy_t* policy,const char* host,int port,nb_flow_policy_t* out);

void nb_policy_init(const char* rules_path);
void nb_flow_policy_default(nb_flow_policy_t* out);
int nb_tiktok_flow_classify(const char* host, int port, nb_flow_policy_t* out);

int nb_prio_is_latency(int prio);
int nb_prio_is_fec_candidate(int prio);

const char* nb_flow_class_name(nb_flow_class_t c);
const char* nb_flow_lane_name(nb_flow_lane_t l);
const char* nb_flow_fec_name(nb_flow_fec_t f);

#endif
