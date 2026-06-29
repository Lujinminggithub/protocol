/* 占位 sidecar：当前产品主链不依赖 sing-box sidecar，保留空实现以保证统一构建稳定。 */

#include <stddef.h>
#include <stdint.h>

typedef struct xgw_runtime_config xgw_runtime_config_t;

int xgw_singbox_sidecar_start(const xgw_runtime_config_t *config, char *error, size_t error_len) {
    (void) config;
    if (error != NULL && error_len > 0U) {
        error[0] = '\0';
    }
    return 1;
}

int xgw_singbox_sidecar_poll(void) {
    return 0;
}

int xgw_singbox_sidecar_dequeue(uint8_t *buf, size_t buf_cap, size_t *out_len, char *error, size_t error_len) {
    (void) buf;
    (void) buf_cap;
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (error != NULL && error_len > 0U) {
        error[0] = '\0';
    }
    return 0;
}

int xgw_singbox_sidecar_handle_payload(const uint8_t *payload, size_t payload_len, char *error, size_t error_len) {
    (void) payload;
    (void) payload_len;
    if (error != NULL && error_len > 0U) {
        error[0] = '\0';
    }
    return 0;
}

void xgw_singbox_sidecar_stop(void) {
}
