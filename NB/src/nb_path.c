#include "nb_path.h"

uint64_t nb_path_sanitize_reorder_delay(uint64_t value){
    return value>UINT64_MAX/2?0:value;
}

uint64_t nb_path_sanitize_reorder_gap(uint64_t value){
    return value>UINT64_MAX/2?0:value;
}

int nb_path_quality_fresh(const nb_path_quality_t* sample,uint64_t now,uint64_t max_age){
    return sample&&sample->sampled_at_us&&now>=sample->sampled_at_us&&now-sample->sampled_at_us<=max_age;
}

int nb_path_quality_sampled(const nb_path_quality_t* sample,uint64_t now,uint64_t max_age,uint64_t minimum){
    return nb_path_quality_fresh(sample,now,max_age)&&sample->sent_packets>=minimum;
}

double nb_path_loss_score(const nb_path_quality_t* sample,uint64_t minimum,double prior){
    if(sample==0||minimum==0)return prior;
    double confidence=(double)sample->sent_packets/(double)minimum;
    if(confidence>1.0)confidence=1.0;
    return sample->effective_loss_pct*confidence+prior*(1.0-confidence);
}

int nb_path_quality_better(const nb_path_quality_t* a,const nb_path_quality_t* b,
    uint64_t now,uint64_t max_age,uint64_t minimum,double prior){
    int af=nb_path_quality_fresh(a,now,max_age),bf=nb_path_quality_fresh(b,now,max_age);
    if(af!=bf)return af>bf;
    if(!af&&!bf)return a->index<b->index;
    double as=nb_path_loss_score(a,minimum,prior),bs=nb_path_loss_score(b,minimum,prior);
    if(as!=bs)return as<bs;
    if(a->jitter_us!=b->jitter_us)return a->jitter_us<b->jitter_us;
    if(a->rtt_us!=b->rtt_us)return a->rtt_us<b->rtt_us;
    return a->index<b->index;
}
