#include "nb_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_policy(const char *host, int port, nb_flow_class_t flow_class,
    nb_flow_lane_t lane, int priority, int matched)
{
    nb_flow_policy_t policy;
    assert(nb_tiktok_flow_classify(host, port, &policy) == matched);
    assert(policy.flow_class == flow_class);
    assert(policy.lane_hint == lane);
    assert(policy.prio == priority);
}

int main(void)
{
    nb_policy_init("/definitely/missing/tiktok_flow_rules.conf");

    assert_policy("live-netacc-fr.tiktokv.com", 443,
        NB_FLOW_CLASS_MEDIA, NB_FLOW_LANE_LATENCY, NB_PRIO_MEDIA, 1);
    assert_policy("203.0.113.8", 50008,
        NB_FLOW_CLASS_MEDIA, NB_FLOW_LANE_LATENCY, NB_PRIO_MEDIA, 1);
    assert_policy("203.0.113.8", 50009,
        NB_FLOW_CLASS_MEDIA, NB_FLOW_LANE_LATENCY, NB_PRIO_MEDIA, 1);
    assert_policy("203.0.113.8", 50000,
        NB_FLOW_CLASS_MEDIA, NB_FLOW_LANE_LATENCY, NB_PRIO_MEDIA, 1);
    assert_policy("203.0.113.8", 50001,
        NB_FLOW_CLASS_MEDIA, NB_FLOW_LANE_LATENCY, NB_PRIO_MEDIA, 1);
    assert_policy("203.0.113.8", 50020,
        NB_FLOW_CLASS_MEDIA, NB_FLOW_LANE_LATENCY, NB_PRIO_MEDIA, 1);
    assert_policy("203.0.113.8", 50021,
        NB_FLOW_CLASS_MEDIA, NB_FLOW_LANE_LATENCY, NB_PRIO_MEDIA, 1);
    assert_policy("203.0.113.8", 50002,
        NB_FLOW_CLASS_BULK, NB_FLOW_LANE_BULK, NB_PRIO_BULK, 0);
    assert_policy("203.0.113.8", 443,
        NB_FLOW_CLASS_BULK, NB_FLOW_LANE_BULK, NB_PRIO_BULK, 1);
    assert_policy("api16-normal-c-useast1a.tiktokv.com", 443,
        NB_FLOW_CLASS_CTRL, NB_FLOW_LANE_LATENCY, NB_PRIO_CTRL, 1);
    assert_policy("pull-f5-sg01.tiktokcdn.com", 443,
        NB_FLOW_CLASS_BULK, NB_FLOW_LANE_BULK, NB_PRIO_BULK, 1);
    assert_policy("ttcdn-useast.tiktokv.com", 443,
        NB_FLOW_CLASS_BULK, NB_FLOW_LANE_BULK, NB_PRIO_BULK, 1);
    assert_policy("sf16-video.tiktokcdn.com", 443,
        NB_FLOW_CLASS_BULK, NB_FLOW_LANE_BULK, NB_PRIO_BULK, 1);
    assert_policy("v16m-default.tiktokcdn.com", 443,
        NB_FLOW_CLASS_BULK, NB_FLOW_LANE_BULK, NB_PRIO_BULK, 1);
    assert_policy("example.invalid", 443,
        NB_FLOW_CLASS_BULK, NB_FLOW_LANE_BULK, NB_PRIO_BULK, 0);

    nb_flow_policy_t policy;
    assert(nb_tiktok_flow_classify("live-netacc.tiktokv.com", 443, &policy) == 1);
    assert(strcmp(policy.rule_name, "live-netacc*") == 0);
    assert(nb_tiktok_flow_classify("203.0.113.8", 443, &policy) == 1);
    assert(strcmp(policy.rule_name, "raw-ip-443") == 0);
    assert(nb_prio_is_latency(NB_PRIO_CTRL));
    assert(nb_prio_is_latency(NB_PRIO_MEDIA));
    assert(!nb_prio_is_latency(NB_PRIO_BULK));
    assert(!nb_prio_is_fec_candidate(NB_PRIO_CTRL));
    assert(nb_prio_is_fec_candidate(NB_PRIO_MEDIA));

    puts("nb_policy tests passed");
    return 0;
}
