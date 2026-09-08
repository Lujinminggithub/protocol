#ifndef NB_YFE2_METRICS_H
#define NB_YFE2_METRICS_H

#include <stddef.h>
#include <stdint.h>

#include "nb_yfe2_adaptive.h"
#include "nb_yfe2_negotiation.h"

typedef struct {
    nb_yfe2_negotiation_state_t negotiation_state;
    int mode;
    uint64_t transitions;
    uint64_t effective_physical_loss;
    uint64_t last_trigger_us;
    uint64_t burst_until_us;
    uint64_t business_bytes;
    uint64_t wire_bytes;
    uint64_t original;
    uint64_t baseline_parity;
    uint64_t burst_parity;
    uint64_t parity_received;
    uint64_t recovered;
    uint64_t unrecoverable;
    uint64_t duplicate;
    uint64_t corrupt;
    uint64_t encode_ns;
    uint64_t decode_ns;
    uint64_t memory_high_bytes;
    uint64_t encoder_queue_drop;
    uint64_t decoder_queue_drop;
    uint64_t parity_queue_drop;
    uint64_t pmtu_fallback;
    uint64_t probe_parity_suppressed;
    uint64_t probe_samples_ignored;
    uint64_t ignored_samples[NB_YFE2_IGNORE_REASON_COUNT];
} nb_yfe2_metrics_t;

void nb_yfe2_metrics_init(nb_yfe2_metrics_t* metrics);
void nb_yfe2_metrics_add(nb_yfe2_metrics_t* destination,
    const nb_yfe2_metrics_t* source);
int nb_yfe2_metrics_render_json(char* out,size_t cap,
    const nb_yfe2_metrics_t* metrics);

#endif
