#ifndef NB_PROBE_H
#define NB_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define NB_PROBE_PROGRESS_INTERVAL_US 10000000ULL
#define NB_PROBE_HASH_INITIAL 14695981039346656037ULL
#define NB_PROBE_HEADER_SIZE 12U
#define NB_PROBE_MAX_BYTES (1ULL << 40)

int nb_probe_progress_format(uint64_t received_bytes, uint64_t now_us,
    uint64_t* last_progress_at, char* output, size_t output_size);
uint64_t nb_probe_hash_update(uint64_t hash, const uint8_t* data, size_t length);
int nb_probe_ack_format(uint64_t received_bytes, uint64_t hash,
    char* output, size_t output_size);
int nb_probe_header_format(uint64_t expected_bytes, uint8_t* output, size_t output_size);
int nb_probe_header_parse(const uint8_t* input, size_t input_size, uint64_t* expected_bytes);
int nb_probe_source_header_format(uint64_t expected_bytes, uint8_t* output, size_t output_size);
int nb_probe_source_header_parse(const uint8_t* input, size_t input_size, uint64_t* expected_bytes);
void nb_probe_source_fill(uint64_t offset,uint8_t* output,size_t length);

#endif
