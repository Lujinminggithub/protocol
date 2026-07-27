#include "nb_lstream.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct {int count;uint64_t next;uint8_t data[40000];size_t length;} receiver_t;
static int receive(void* ctx,const nb_lstream_frame_t* f){receiver_t* r=ctx;r->count++;if(f->type==NB_LSTREAM_DATA){if(f->offset!=r->next)return -1;
    memcpy(r->data+r->length,f->payload,f->payload_length);r->length+=f->payload_length;r->next+=f->payload_length;}return 0;}
int main(void){uint64_t hi=0x0102030405060708ULL,lo=0x1112131415161718ULL;char flow[33];CHECK(nb_lstream_flow_render(flow,hi,lo)==0);
    uint64_t a=0,b=0;CHECK(nb_lstream_flow_parse(flow,&a,&b)==0&&a==hi&&b==lo);nb_lstream_tx_t tx;nb_lstream_tx_init(&tx,hi,lo);
    uint8_t source[32777];for(size_t i=0;i<sizeof(source);i++)source[i]=(uint8_t)(i*17u);uint64_t offset=99;
    CHECK(nb_lstream_tx_append(&tx,source,sizeof(source),&offset)==0&&offset==0&&tx.next_offset==sizeof(source));
    CHECK(nb_lstream_tx_ack(&tx,12345)==0&&tx.base_offset==12345&&tx.replay_length==sizeof(source)-12345);
    CHECK(memcmp(tx.replay,source+12345,tx.replay_length)==0);CHECK(nb_lstream_tx_ack(&tx,tx.next_offset+1)!=0);
    CHECK(nb_lstream_tx_fin(&tx)==0&&tx.fin_offset==sizeof(source));uint8_t frames[40000];size_t wire_length=0;
    for(size_t pos=0;pos<sizeof(source);){size_t n=sizeof(source)-pos;if(n>NB_LSTREAM_DATA_MAX)n=NB_LSTREAM_DATA_MAX;
        int encoded=nb_lstream_frame_encode(frames+wire_length,sizeof(frames)-wire_length,NB_LSTREAM_DATA,hi,lo,pos,source+pos,(uint32_t)n);
        CHECK(encoded>0);wire_length+=(size_t)encoded;pos+=n;}nb_lstream_decoder_t decoder;nb_lstream_decoder_init(&decoder);receiver_t receiver={0};
    for(size_t pos=0;pos<wire_length;){size_t n=(pos%37)+1;if(n>wire_length-pos)n=wire_length-pos;
        CHECK(nb_lstream_decoder_feed(&decoder,frames+pos,n,receive,&receiver)==0);pos+=n;}
    CHECK(receiver.count==3&&receiver.length==sizeof(source)&&memcmp(receiver.data,source,sizeof(source))==0);
    frames[0]=0;nb_lstream_decoder_init(&decoder);CHECK(nb_lstream_decoder_feed(&decoder,frames,NB_LSTREAM_HEADER,receive,&receiver)!=0);
    nb_lstream_tx_dispose(&tx);puts("nb_lstream_test: ok");return 0;}
