#include "nb_yfe2_metrics.h"

#include <stdio.h>
#include <string.h>

static void add_saturated(uint64_t* destination,uint64_t value){
    *destination=UINT64_MAX-*destination<value?UINT64_MAX:*destination+value;
}

void nb_yfe2_metrics_init(nb_yfe2_metrics_t* metrics){
    if(metrics==NULL)return;
    memset(metrics,0,sizeof(*metrics));
    metrics->negotiation_state=NB_YFE2_NEG_DISABLED;
    metrics->mode=NB_YFE2_MODE_BASELINE;
}

void nb_yfe2_metrics_add(nb_yfe2_metrics_t* destination,
    const nb_yfe2_metrics_t* source){
    if(destination==NULL||source==NULL)return;
    if(source->negotiation_state==NB_YFE2_NEG_ACCEPTED||
        (destination->negotiation_state!=NB_YFE2_NEG_ACCEPTED&&
        source->negotiation_state>destination->negotiation_state))
        destination->negotiation_state=source->negotiation_state;
    if(source->mode==NB_YFE2_MODE_BURST)destination->mode=source->mode;
    add_saturated(&destination->transitions,source->transitions);
    add_saturated(&destination->effective_physical_loss,source->effective_physical_loss);
    if(source->last_trigger_us>destination->last_trigger_us)
        destination->last_trigger_us=source->last_trigger_us;
    if(source->burst_until_us>destination->burst_until_us)
        destination->burst_until_us=source->burst_until_us;
#define NB_YFE2_ADD(field) add_saturated(&destination->field,source->field)
    NB_YFE2_ADD(business_bytes);NB_YFE2_ADD(wire_bytes);NB_YFE2_ADD(original);
    NB_YFE2_ADD(baseline_parity);NB_YFE2_ADD(burst_parity);NB_YFE2_ADD(parity_received);
    NB_YFE2_ADD(recovered);NB_YFE2_ADD(unrecoverable);NB_YFE2_ADD(duplicate);
    NB_YFE2_ADD(corrupt);NB_YFE2_ADD(encode_ns);NB_YFE2_ADD(decode_ns);
    if(source->memory_high_bytes>destination->memory_high_bytes)
        destination->memory_high_bytes=source->memory_high_bytes;
    NB_YFE2_ADD(encoder_queue_drop);NB_YFE2_ADD(decoder_queue_drop);
    NB_YFE2_ADD(parity_queue_drop);NB_YFE2_ADD(pmtu_fallback);
    NB_YFE2_ADD(probe_parity_suppressed);NB_YFE2_ADD(probe_samples_ignored);
#undef NB_YFE2_ADD
    for(size_t i=0;i<NB_YFE2_IGNORE_REASON_COUNT;i++)
        add_saturated(&destination->ignored_samples[i],source->ignored_samples[i]);
}

int nb_yfe2_metrics_render_json(char* out,size_t cap,
    const nb_yfe2_metrics_t* metrics){
    if(out==NULL||cap==0||metrics==NULL)return -1;
    double overhead=0.0;
    if(metrics->business_bytes&&metrics->wire_bytes>metrics->business_bytes)
        overhead=(double)(metrics->wire_bytes-metrics->business_bytes)/
            (double)metrics->business_bytes;
    return snprintf(out,cap,
        "{\"negotiation\":{\"state\":\"%s\",\"codec\":\"nb-yfe2\",\"wire_version\":3,\"profile_id\":%u},"
        "\"mode\":{\"current\":\"%s\",\"transitions\":%llu,\"effective_physical_loss\":%llu,\"last_trigger_us\":%llu,\"burst_until_us\":%llu},"
        "\"traffic\":{\"business_bytes\":%llu,\"wire_bytes\":%llu,\"overhead_ratio\":%.6f},"
        "\"shards\":{\"original\":%llu,\"baseline_parity\":%llu,\"burst_parity\":%llu,\"received\":%llu,\"recovered\":%llu,\"unrecoverable\":%llu,\"duplicate\":%llu,\"corrupt\":%llu},"
        "\"resources\":{\"memory_high_bytes\":%llu,\"encoder_queue_drop\":%llu,\"decoder_queue_drop\":%llu,\"parity_queue_drop\":%llu,\"pmtu_fallback\":%llu,\"encode_ns\":%llu,\"decode_ns\":%llu},"
        "\"ignored\":{\"local_drop\":%llu,\"scheduler\":%llu,\"send_queue\":%llu,\"pmtu\":%llu,\"app_limited\":%llu,\"rate_cap\":%llu,\"probe\":%llu,\"probe_parity_suppressed\":%llu,\"probe_samples_ignored\":%llu}}",
        nb_yfe2_negotiation_state_name(metrics->negotiation_state),
        NB_YFE2_PROFILE_ID,metrics->mode==NB_YFE2_MODE_BURST?"burst":"baseline",
        (unsigned long long)metrics->transitions,
        (unsigned long long)metrics->effective_physical_loss,
        (unsigned long long)metrics->last_trigger_us,
        (unsigned long long)metrics->burst_until_us,
        (unsigned long long)metrics->business_bytes,
        (unsigned long long)metrics->wire_bytes,overhead,
        (unsigned long long)metrics->original,
        (unsigned long long)metrics->baseline_parity,
        (unsigned long long)metrics->burst_parity,
        (unsigned long long)metrics->parity_received,
        (unsigned long long)metrics->recovered,
        (unsigned long long)metrics->unrecoverable,
        (unsigned long long)metrics->duplicate,
        (unsigned long long)metrics->corrupt,
        (unsigned long long)metrics->memory_high_bytes,
        (unsigned long long)metrics->encoder_queue_drop,
        (unsigned long long)metrics->decoder_queue_drop,
        (unsigned long long)metrics->parity_queue_drop,
        (unsigned long long)metrics->pmtu_fallback,
        (unsigned long long)metrics->encode_ns,
        (unsigned long long)metrics->decode_ns,
        (unsigned long long)metrics->ignored_samples[0],
        (unsigned long long)metrics->ignored_samples[1],
        (unsigned long long)metrics->ignored_samples[2],
        (unsigned long long)metrics->ignored_samples[3],
        (unsigned long long)metrics->ignored_samples[4],
        (unsigned long long)metrics->ignored_samples[5],
        (unsigned long long)metrics->ignored_samples[6],
        (unsigned long long)metrics->probe_parity_suppressed,
        (unsigned long long)metrics->probe_samples_ignored);
}
