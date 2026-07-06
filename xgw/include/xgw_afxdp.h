#ifndef XGW_AFXDP_H
#define XGW_AFXDP_H

/* AF_XDP 相关定义，包含 UMEM、ring、BPF pin 路径等运行状态。 */

#include <stddef.h>
#include <stdint.h>

/* 单个 AF_XDP socket 的运行时状态。 */
typedef struct xgw_afxdp_socket {
    int ready;
    char device[64];
    uint32_t queue_id;
    uint16_t port;
    int fd;
    uint32_t ifindex;
    uint32_t frame_size;
    uint32_t frame_count;
    uint32_t rx_ring_size;
    uint32_t tx_ring_size;
    uint32_t fill_ring_size;
    uint32_t comp_ring_size;
    uint32_t rx_mask;
    uint32_t tx_mask;
    uint32_t fill_mask;
    uint32_t comp_mask;
    uint8_t *umem_area;
    size_t umem_len;
    void *rx_map;
    size_t rx_map_len;
    void *tx_map;
    size_t tx_map_len;
    void *fill_map;
    size_t fill_map_len;
    void *comp_map;
    size_t comp_map_len;
    void *rx_producer;
    void *rx_consumer;
    void *rx_flags;
    uint8_t *rx_desc_base;
    void *tx_producer;
    void *tx_consumer;
    void *tx_flags;
    uint8_t *tx_desc_base;
    void *fill_producer;
    void *fill_consumer;
    void *fill_flags;
    uint8_t *fill_desc_base;
    void *comp_producer;
    void *comp_consumer;
    void *comp_flags;
    uint8_t *comp_desc_base;
    uint64_t *free_frames;
    uint32_t free_count;
    uint32_t free_cap;
    char pin_root[160];
    char prog_pin[200];
    char map_pin[200];
    char config_map_pin[200];
    char bpf_src[260];
    char bpf_obj[260];
} xgw_afxdp_socket_t;

/* 打开并初始化 AF_XDP socket。 */
int xgw_afxdp_open(xgw_afxdp_socket_t *sock, const char *device, uint32_t queue_id, uint16_t port, char *error, size_t error_len);
/* 从 AF_XDP 接收数据。 */
int xgw_afxdp_recv(xgw_afxdp_socket_t *sock, uint8_t *buf, size_t buf_cap, size_t *out_len);
/* 通过 AF_XDP 发送数据。 */
int xgw_afxdp_send(xgw_afxdp_socket_t *sock, const uint8_t *buf, size_t buf_len);
/* 关闭 AF_XDP socket 并回收资源。 */
void xgw_afxdp_close(xgw_afxdp_socket_t *sock);

#endif
