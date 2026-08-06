#include "nb_metrics.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
    nb_metrics_state_t state={0};
    nb_metrics_note_close(&state,"idle-timeout");nb_metrics_note_close(&state,"target-connect-fail");
    nb_metrics_note_close(&state,"bidirectional-fin");nb_metrics_note_close(&state,"quic-stream-reset");
    nb_metrics_note_loop(&state,6000,3000);nb_metrics_note_loop(&state,25000,40000);
    nb_metrics_note_udp_error(&state,0);nb_metrics_note_udp_error(&state,1);
    nb_metrics_note_udp_rxq_overflow(&state,7);
    nb_metrics_note_udp_queue_pressure_drop(&state,3);
    state.pool_retire_total=2;state.pool_retire_suppressed=3;
    state.pool_quarantine_total=4;state.pool_mbb_promotions=2;
    state.pmtu_promotions=4;state.pmtu_fallbacks=1;
    nb_metrics_snapshot_t snapshot;
    nb_metrics_snapshot_init(&snapshot,&state,2,4,1,1);
    nb_metrics_snapshot_add_stream(&snapshot,2,1,0,1,1,0,100,200,10,20,30,1000,2000,3000,4000,5000);
    nb_metrics_snapshot_add_stream(&snapshot,1,0,1,0,0,1,300,400,5,6,7,500,600,700,1000,2000);
    assert(snapshot.media_sessions==1&&snapshot.ctrl_sessions==1&&snapshot.udp_sessions==1);
    assert(snapshot.bytes_c2s==400&&snapshot.queue_q2t_bytes==37&&snapshot.queue_q2t_age_max_us==3000);
    assert(snapshot.target_connect_failed==1&&snapshot.first_s2c_wait_max_us==5000);
    nb_metrics_snapshot_add_link(&snapshot,3.5,200000,12000,200);
    nb_metrics_snapshot_add_transport(&snapshot,"bbr",2,10,4,2,24,100000,1000,500,9000,1);
    nb_metrics_snapshot_add_seed(&snapshot,1,281000,393216);
    assert(snapshot.link_samples==1&&snapshot.link_effective_loss_max_pct==3.5);
    assert(state.close_total==4&&state.close_timeout==1&&state.close_error==1&&state.close_reset==1&&state.close_normal==1);
    nb_metrics_fec_t fec={.observe=1,.active=0,.tx_blocks=2,.rx_blocks=3,
        .recovered=4,.nack=5,.retx=6};char json[4096];
    int len=nb_metrics_render_json(json,sizeof(json),"middle","0","abc","line",2,&snapshot,&fec);
    assert(len>0&&(size_t)len<sizeof(json));assert(strstr(json,"\"media\":1")!=NULL);
    assert(strstr(json,"\"busy_max_us\":25000")!=NULL);assert(strstr(json,"\"tx_blocks\":2")!=NULL);
    assert(strstr(json,"\"udp_errors\":{\"rx\":1,\"tx\":1,\"rxq_overflow\":7,\"queue_pressure_dropped\":3}")!=NULL);
    assert(strstr(json,"\"bdp_seed\":{\"configured\":1,\"applied\":1,\"rtt_us_max\":281000,\"cwin_bytes_max\":393216}")!=NULL);
    assert(strstr(json,"\"spurious_total\":4")!=NULL&&strstr(json,"\"cc\":\"bbr\"")!=NULL);
    assert(strstr(json,"\"pool_recovery\":{\"retired\":2,\"suppressed\":3,\"quarantined\":4,\"mbb_promotions\":2}")!=NULL);
    assert(strstr(json,"\"pmtu\":{\"promotions\":4,\"fallbacks\":1}")!=NULL);
    puts("RESULT PASS");return 0;
}
