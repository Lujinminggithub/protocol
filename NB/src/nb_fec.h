#ifndef NB_FEC_H
#define NB_FEC_H

#include <stddef.h>
#include <stdint.h>

#define NB_FEC_SYMBOL_SIZE 960u
#define NB_FEC_MAX_K 8u
#define NB_FEC_MAX_R 4u
#define NB_FEC_PROTOCOL_VERSION 2u
#define NB_FEC_DEFAULT_BLOCK_HOLD_US 1000u

typedef enum {
    NB_FEC_OK = 0,
    NB_FEC_ERR_ARGUMENT = -1,
    NB_FEC_ERR_MEMORY = -2,
    NB_FEC_ERR_PROTOCOL = -3,
    NB_FEC_ERR_IO = -4,
    NB_FEC_ERR_WINDOW = -5,
    NB_FEC_ERR_RETRY_EXHAUSTED = -6
} nb_fec_result_t;

typedef struct {
    uint8_t k;
    uint8_t r;
    uint16_t symbol_size;
    uint32_t tx_history_blocks;
    uint32_t rx_window_blocks;
    uint64_t block_hold_us;
    uint64_t nack_delay_us;
    uint64_t nack_retry_us;
    uint8_t max_nack_retries;
} nb_fec_config_t;

typedef struct {
    uint64_t source_packets;
    uint64_t repair_packets;
    uint64_t source_bytes;
    uint64_t repair_bytes;
    uint64_t blocks_encoded;
    uint64_t blocks_delivered;
    uint64_t blocks_recovered;
    uint64_t recovered_source_bytes;
    uint64_t delivered_bytes;
    uint64_t out_of_order_packets;
    uint64_t duplicate_packets;
    uint64_t nack_sent;
    uint64_t nack_retried;
    uint64_t retx_sent;
    uint64_t retx_received;
    uint64_t retx_acked;
    uint64_t block_acked;
    uint64_t protocol_errors;
    uint64_t window_errors;
    uint64_t retry_exhausted;
} nb_fec_metrics_t;

typedef struct {
    int (*send_datagram)(void* ctx, const uint8_t* data, size_t len);
    int (*send_control)(void* ctx, const uint8_t* data, size_t len);
    int (*deliver)(void* ctx, const uint8_t* data, size_t len, int fin);
} nb_fec_callbacks_t;

typedef struct nb_fec_session nb_fec_session_t;

/* history/window = ceil((bitrate * RTT * multiplier) / block_bytes), with safe bounds. */
int nb_fec_config_for_bdp(nb_fec_config_t* cfg, uint8_t k, uint8_t r,
    uint64_t bitrate_bps, uint64_t rtt_us, unsigned multiplier);

nb_fec_session_t* nb_fec_session_create(uint32_t session_id,
    const nb_fec_config_t* cfg, const nb_fec_callbacks_t* cb, void* cb_ctx);
void nb_fec_session_destroy(nb_fec_session_t* s);

int nb_fec_tx_feed(nb_fec_session_t* s, const uint8_t* data, size_t len,
    int fin, uint64_t now_us);
size_t nb_fec_tx_writable(const nb_fec_session_t* s);
int nb_fec_tx_finish_ready(const nb_fec_session_t* s);
int nb_fec_tx_complete(const nb_fec_session_t* s);
int nb_fec_on_datagram(nb_fec_session_t* s, const uint8_t* data, size_t len,
    uint64_t now_us);
int nb_fec_on_control(nb_fec_session_t* s, const char* line, size_t len,
    uint64_t now_us);
int nb_fec_tick(nb_fec_session_t* s, uint64_t now_us);
int nb_fec_send_reset(nb_fec_session_t* s, uint32_t code);

uint64_t nb_fec_next_deadline(const nb_fec_session_t* s);
const nb_fec_metrics_t* nb_fec_metrics(const nb_fec_session_t* s);
int nb_fec_failed(const nb_fec_session_t* s);

#endif
