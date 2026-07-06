#ifndef XGW_TUN_H
#define XGW_TUN_H

/* TUN 设备抽象。 */

#include <stddef.h>
#include <stdint.h>

/* 打开的 TUN 设备句柄。 */
typedef struct xgw_tun_device {
    int fd;
    char name[64];
} xgw_tun_device_t;

/* 打开并配置一个 TUN 设备。 */
int xgw_tun_open(xgw_tun_device_t *dev, const char *name, const char *addr, uint32_t mtu, char *error, size_t error_len);
/* 从 TUN 读取一个包。 */
int xgw_tun_read(xgw_tun_device_t *dev, uint8_t *buf, size_t buf_cap, size_t *out_len);
/* 向 TUN 写入一个包。 */
int xgw_tun_write(xgw_tun_device_t *dev, const uint8_t *buf, size_t buf_len);
/* 关闭 TUN 设备。 */
void xgw_tun_close(xgw_tun_device_t *dev);

#endif
