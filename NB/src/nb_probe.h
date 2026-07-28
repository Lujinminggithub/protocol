#ifndef NB_PROBE_H
#define NB_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define NB_PROBE_PROGRESS_INTERVAL_US 10000000ULL

int nb_probe_progress_format(uint64_t received_bytes, uint64_t now_us,
    uint64_t* last_progress_at, char* output, size_t output_size);

#endif
