#ifndef NB_LSTREAM_H
#define NB_LSTREAM_H

#include <stddef.h>
#include <stdint.h>

#define NB_LSTREAM_MAGIC 0x4e424c53u
#define NB_LSTREAM_VERSION 1
#define NB_LSTREAM_HEADER 36
#define NB_LSTREAM_DATA_MAX 16384
#define NB_LSTREAM_REPLAY_MAX (1024u*1024u)

enum { NB_LSTREAM_OPEN=1, NB_LSTREAM_DATA=2, NB_LSTREAM_ACK=3,
    NB_LSTREAM_RESUME=4, NB_LSTREAM_RESUME_ACK=5, NB_LSTREAM_FIN=6,
    NB_LSTREAM_RST=7, NB_LSTREAM_FIN_ACK=8 };

typedef struct { uint8_t type;uint64_t flow_hi;uint64_t flow_lo;uint64_t offset;
    const uint8_t* payload;uint32_t payload_length; } nb_lstream_frame_t;
typedef int (*nb_lstream_frame_fn)(void* ctx,const nb_lstream_frame_t* frame);
typedef struct { uint8_t wire[NB_LSTREAM_HEADER+NB_LSTREAM_DATA_MAX];size_t length;size_t expected; } nb_lstream_decoder_t;
typedef struct { uint64_t flow_hi;uint64_t flow_lo;uint64_t base_offset;uint64_t next_offset;
    uint8_t* replay;size_t replay_length;size_t replay_capacity;int fin_queued;int fin_acked;uint64_t fin_offset; } nb_lstream_tx_t;

int nb_lstream_type_valid(uint8_t type);
int nb_lstream_frame_encode(uint8_t* out,size_t cap,uint8_t type,uint64_t flow_hi,
    uint64_t flow_lo,uint64_t offset,const uint8_t* payload,uint32_t payload_length);
int nb_lstream_frame_decode(const uint8_t* wire,size_t length,nb_lstream_frame_t* out);
void nb_lstream_decoder_init(nb_lstream_decoder_t* decoder);
int nb_lstream_decoder_feed(nb_lstream_decoder_t* decoder,const uint8_t* data,size_t length,
    nb_lstream_frame_fn callback,void* ctx);
void nb_lstream_tx_init(nb_lstream_tx_t* tx,uint64_t flow_hi,uint64_t flow_lo);
void nb_lstream_tx_dispose(nb_lstream_tx_t* tx);
int nb_lstream_tx_append(nb_lstream_tx_t* tx,const uint8_t* data,size_t length,uint64_t* offset);
int nb_lstream_tx_ack(nb_lstream_tx_t* tx,uint64_t offset);
int nb_lstream_tx_fin(nb_lstream_tx_t* tx);
int nb_lstream_flow_render(char out[33],uint64_t hi,uint64_t lo);
int nb_lstream_flow_parse(const char* text,uint64_t* hi,uint64_t* lo);

#endif
