#ifndef NB_PATH_H
#define NB_PATH_H

#include <stdint.h>
typedef struct {
    double effective_loss_pct;
    uint64_t rtt_us;
    uint64_t jitter_us;
    uint64_t sent_packets;
    uint64_t sampled_at_us;
    int index;
} nb_path_quality_t;

int nb_path_quality_fresh(const nb_path_quality_t* sample,uint64_t now_us,uint64_t max_age_us);
int nb_path_quality_sampled(const nb_path_quality_t* sample,uint64_t now_us,uint64_t max_age_us,uint64_t minimum_packets);
double nb_path_loss_score(const nb_path_quality_t* sample,uint64_t minimum_packets,double prior_pct);
int nb_path_quality_better(const nb_path_quality_t* candidate,const nb_path_quality_t* current,
    uint64_t now_us,uint64_t max_age_us,uint64_t minimum_packets,double prior_pct);
uint64_t nb_path_sanitize_reorder_delay(uint64_t value);
uint64_t nb_path_sanitize_reorder_gap(uint64_t value);

#endif
