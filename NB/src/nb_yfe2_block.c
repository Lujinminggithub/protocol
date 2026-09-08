#include "nb_yfe2_block.h"

#include "nb_fec_rs.h"

#include <string.h>

static int parity_valid(uint8_t value){return value==NB_YFE2_BASE_PARITY||value==NB_YFE2_BURST_PARITY;}
static int source_valid(const nb_udp_wire_view_t* source){return source&&source->type==NB_UDP_TYPE_C2S&&
    source->session_id&&source->payload&&source->payload_length&&source->payload_length<=NB_UDP_FRAGMENT_PAYLOAD&&
    source->fragment_count&&source->fragment_index<source->fragment_count&&source->total_length;}

void nb_yfe2_tx_init(nb_yfe2_tx_t* state,uint64_t generation,uint32_t session_id){
    if(state){memset(state,0,sizeof(*state));state->connection_generation=generation;
        state->session_id=session_id;state->next_block_id=1;state->parity_shards=NB_YFE2_BASE_PARITY;}
}
void nb_yfe2_tx_set_parity(nb_yfe2_tx_t* state,uint8_t parity_shards){if(state&&parity_valid(parity_shards))state->parity_shards=parity_shards;}
size_t nb_yfe2_tx_slot_count(const nb_yfe2_tx_t* state,size_t slot){return state&&slot<NB_YFE2_INTERLEAVE?state->slots[slot].job.actual_count:0;}
uint64_t nb_yfe2_tx_next_deadline(const nb_yfe2_tx_t* state){
    uint64_t earliest=0;if(state==NULL)return 0;
    for(size_t i=0;i<NB_YFE2_INTERLEAVE;i++)if(state->slots[i].active){uint64_t deadline=state->slots[i].first_at+NB_YFE2_FLUSH_US;
        if(earliest==0||deadline<earliest)earliest=deadline;}
    return earliest;
}
static void slot_begin(nb_yfe2_tx_t* state,nb_yfe2_tx_slot_t* slot,uint64_t now,uint8_t parity){
    memset(slot,0,sizeof(*slot));slot->active=1;slot->first_at=now;
    slot->job.connection_generation=state->connection_generation;slot->job.session_id=state->session_id;
    slot->job.block_id=state->next_block_id++;slot->job.parity_shards=parity;
}
static int slot_seal(nb_yfe2_tx_slot_t* slot,nb_yfe2_encode_job_t* sealed){
    if(slot==NULL||sealed==NULL||!slot->active||slot->job.actual_count==0)return -1;
    *sealed=slot->job;memset(slot,0,sizeof(*slot));return 1;
}
int nb_yfe2_tx_add(nb_yfe2_tx_t* state,const nb_udp_wire_view_t* source,uint64_t now_us,
    uint8_t parity_shards,nb_yfe2_encode_job_t* sealed){
    if(state==NULL||sealed==NULL||!source_valid(source)||source->session_id!=state->session_id)return -1;
    if(parity_shards==0)parity_shards=state->parity_shards;
    if(!parity_valid(parity_shards))return -1;
    size_t index=state->next_slot++%NB_YFE2_INTERLEAVE;nb_yfe2_tx_slot_t* slot=&state->slots[index];
    if(!slot->active)slot_begin(state,slot,now_us,parity_shards);
    uint8_t item=slot->job.actual_count;if(item>=NB_YFE2_DATA_SHARDS)return -1;
    slot->job.desc[item]=(nb_yfe2_desc_t){source->sequence,source->fragment_index,source->fragment_count,
        source->total_length,source->payload_length};
    memcpy(slot->job.source[item],source->payload,source->payload_length);
    if(source->payload_length>slot->job.shard_size)slot->job.shard_size=source->payload_length;
    slot->job.actual_count++;
    return slot->job.actual_count==NB_YFE2_DATA_SHARDS?slot_seal(slot,sealed):0;
}
int nb_yfe2_tx_flush_due(nb_yfe2_tx_t* state,uint64_t now_us,nb_yfe2_encode_job_t* sealed){
    if(state==NULL||sealed==NULL)return -1;
    size_t selected=NB_YFE2_INTERLEAVE;uint64_t oldest=UINT64_MAX;
    for(size_t i=0;i<NB_YFE2_INTERLEAVE;i++){nb_yfe2_tx_slot_t* slot=&state->slots[i];
        if(slot->active&&now_us>=slot->first_at&&now_us-slot->first_at>=NB_YFE2_FLUSH_US&&slot->first_at<oldest){selected=i;oldest=slot->first_at;}}
    return selected==NB_YFE2_INTERLEAVE?0:slot_seal(&state->slots[selected],sealed);
}
int nb_yfe2_encode_job_run(const nb_yfe2_encode_job_t* job,nb_yfe2_encode_result_t* result){
    if(job==NULL||result==NULL||job->connection_generation==0||job->session_id==0||job->actual_count==0||
        job->actual_count>NB_YFE2_DATA_SHARDS||job->shard_size==0||job->shard_size>NB_UDP_FRAGMENT_PAYLOAD||
        !parity_valid(job->parity_shards))return -1;
    uint8_t source[NB_YFE2_DATA_SHARDS][NB_UDP_FRAGMENT_PAYLOAD]={{0}};
    uint8_t repair[NB_YFE2_BURST_PARITY][NB_UDP_FRAGMENT_PAYLOAD]={{0}};
    uint8_t* src[NB_YFE2_DATA_SHARDS];uint8_t* rep[NB_YFE2_BURST_PARITY];
    for(size_t i=0;i<NB_YFE2_DATA_SHARDS;i++){src[i]=source[i];if(i<job->actual_count)memcpy(source[i],job->source[i],job->desc[i].payload_length);}
    for(uint8_t i=0;i<job->parity_shards;i++)rep[i]=repair[i];
    if(nb_rs_encode(NB_YFE2_DATA_SHARDS,job->parity_shards,src,rep,job->shard_size)!=0)return -1;
    memset(result,0,sizeof(*result));result->connection_generation=job->connection_generation;
    result->session_id=job->session_id;result->block_id=job->block_id;result->parity_count=job->parity_shards;
    uint8_t flags=job->parity_shards==NB_YFE2_BURST_PARITY?NB_YFE2_FLAG_BURST:0;
    for(uint8_t i=0;i<job->parity_shards;i++){
        nb_yfe2_parity_t parity={flags,NB_YFE2_DIRECTION_C2S,(uint8_t)(NB_YFE2_DATA_SHARDS+i),
            NB_YFE2_DATA_SHARDS,job->parity_shards,job->actual_count,NB_YFE2_PROFILE_ID,job->shard_size,
            job->session_id,job->block_id,job->desc,repair[i]};
        int length=nb_yfe2_wire_encode(result->parity[i].wire,sizeof(result->parity[i].wire),&parity);
        if(length<0)return -1;
        result->parity[i].length=(size_t)length;
    }
    return 0;
}
