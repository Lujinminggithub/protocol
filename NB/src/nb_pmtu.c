#include "nb_pmtu.h"

#define NB_PMTU_CONFIRMATIONS 3U
#define NB_PMTU_COOLDOWN_US 60000000ULL

static size_t clamp_mtu(const nb_pmtu_state_t* state,size_t mtu){
    if(mtu<state->floor)return state->floor;
    if(mtu>state->ceiling)return state->ceiling;
    return mtu;
}

void nb_pmtu_init(nb_pmtu_state_t* state,size_t floor,size_t ceiling){
    if(state==0)return;
    if(floor<1200)floor=1200;
    if(ceiling<floor)ceiling=floor;
    *state=(nb_pmtu_state_t){.floor=floor,.ceiling=ceiling,.confirmed=floor};
}

int nb_pmtu_observe(nb_pmtu_state_t* state,size_t path_mtu,
    uint64_t timer_losses,uint64_t spurious_losses,uint64_t now_us){
    if(state==0||state->floor==0||path_mtu==0)return 0;
    path_mtu=clamp_mtu(state,path_mtu);
    uint64_t effective_timer=timer_losses>spurious_losses?timer_losses-spurious_losses:0;
    if(path_mtu<state->confirmed){
        state->confirmed=path_mtu;state->candidate=0;state->confirmations=0;
        state->cooldown_until_us=now_us+NB_PMTU_COOLDOWN_US;state->fallbacks++;
        return -1;
    }
    if(effective_timer>=2){state->candidate=0;state->confirmations=0;return 0;}
    if(path_mtu<=state->confirmed||now_us<state->cooldown_until_us){
        state->candidate=0;state->confirmations=0;return 0;
    }
    if(path_mtu!=state->candidate){state->candidate=path_mtu;state->confirmations=1;return 0;}
    if(++state->confirmations<NB_PMTU_CONFIRMATIONS)return 0;
    state->confirmed=state->candidate;state->candidate=0;state->confirmations=0;state->promotions++;
    return 1;
}

size_t nb_pmtu_effective(const nb_pmtu_state_t* state){return state?state->confirmed:0;}
