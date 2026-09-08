#include "nb_yfe2_metrics.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){ \
    fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1; } } while(0)

int main(void){
    nb_yfe2_metrics_t metrics;nb_yfe2_metrics_init(&metrics);
    metrics.negotiation_state=NB_YFE2_NEG_ACCEPTED;
    metrics.mode=NB_YFE2_MODE_BASELINE;metrics.transitions=2;
    metrics.effective_physical_loss=3;
    metrics.business_bytes=16000000;metrics.wire_bytes=17000000;
    metrics.original=16000;metrics.baseline_parity=1000;metrics.burst_parity=30;
    metrics.recovered=4;metrics.unrecoverable=1;metrics.duplicate=2;
    metrics.memory_high_bytes=1048576;metrics.encoder_queue_drop=0;
    metrics.decoder_queue_drop=1;metrics.encode_ns=UINT64_MAX;
    char json[4096];CHECK(nb_yfe2_metrics_render_json(json,sizeof(json),&metrics)>0);
    CHECK(strstr(json,"\"state\":\"accepted\"")!=NULL);
    CHECK(strstr(json,"\"codec\":\"nb-yfe2\"")!=NULL);
    CHECK(strstr(json,"\"wire_version\":3")!=NULL);
    CHECK(strstr(json,"\"profile_id\":28909")!=NULL);
    CHECK(strstr(json,"\"overhead_ratio\":0.062500")!=NULL);
    CHECK(strstr(json,"\"encode_ns\":18446744073709551615")!=NULL);
    metrics.business_bytes=0;metrics.wire_bytes=0;
    CHECK(nb_yfe2_metrics_render_json(json,sizeof(json),&metrics)>0);
    CHECK(strstr(json,"\"overhead_ratio\":0.000000")!=NULL);
    puts("nb_yfe2_metrics_test: ok");return 0;
}
