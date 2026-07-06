/* Linux TUN 实现：负责创建设备、配置地址、应用 MTU 并提供读写能力。 */

#ifdef __linux__

#include "xgw_tun.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void set_error(char *error, size_t error_len, const char *text) {
    if (error_len > 0U) {
        snprintf(error, error_len, "%s", text == NULL ? "" : text);
    }
}

static void set_error_errno(char *error, size_t error_len, const char *context) {
    int e = errno;
    const char *hint = "";
    if (e == EPERM || e == EACCES) {
        hint = " (need CAP_NET_ADMIN or privileged access)";
    } else if (e == ENOENT) {
        hint = " (/dev/net/tun may be missing)";
    } else if (e == ENODEV) {
        hint = " (TUN/TAP device support may be unavailable)";
    }
    if (error_len > 0U) {
        snprintf(error, error_len, "%s: errno=%d (%s)%s", context, e, strerror(e), hint);
    }
}

static int run_cmd_text(const char *cmd) {
    return system(cmd) == 0;
}

static int run_cmd(const char *fmt, const char *arg1, unsigned int arg2) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), fmt, arg1 == NULL ? "" : arg1, arg2);
    return run_cmd_text(cmd);
}

static int try_set_mtu_candidates(const char *dev_name, uint32_t requested_mtu, uint32_t *applied_mtu) {
    static const uint32_t fallback_candidates[] = {1380U, 1360U, 1320U, 1280U, 1240U, 1200U};
    uint32_t candidates[8];
    size_t count = 0;
    size_t i;

    if (requested_mtu > 0U) {
        candidates[count++] = requested_mtu;
    }
    for (i = 0; i < sizeof(fallback_candidates) / sizeof(fallback_candidates[0]); ++i) {
        uint32_t mtu = fallback_candidates[i];
        size_t j;
        int exists = 0;
        for (j = 0; j < count; ++j) {
            if (candidates[j] == mtu) {
                exists = 1;
                break;
            }
        }
        if (!exists) {
            candidates[count++] = mtu;
        }
    }

    for (i = 0; i < count; ++i) {
        if (run_cmd("ip link set dev %s mtu %u", dev_name, candidates[i])) {
            if (applied_mtu != NULL) {
                *applied_mtu = candidates[i];
            }
            return 1;
        }
    }
    return 0;
}

/* 打开并配置一个 Linux TUN 设备。 */
int xgw_tun_open(xgw_tun_device_t *dev, const char *name, const char *addr, uint32_t mtu, char *error, size_t error_len) {
    struct ifreq ifr;
    int fd;
    uint32_t applied_mtu = 0;
    memset(dev, 0, sizeof(*dev));
    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        set_error_errno(error, error_len, "open /dev/net/tun failed");
        return 0;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(fd, TUNSETIFF, (void *) &ifr) < 0) {
        close(fd);
        set_error_errno(error, error_len, "TUNSETIFF failed");
        return 0;
    }
    dev->fd = fd;
    snprintf(dev->name, sizeof(dev->name), "%s", ifr.ifr_name);
    if (mtu > 0U) {
        if (!try_set_mtu_candidates(dev->name, mtu, &applied_mtu)) {
            set_error(error, error_len, "ip link mtu fallback failed");
            xgw_tun_close(dev);
            return 0;
        }
    }
    if (addr != NULL && addr[0] != '\0') {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip address replace %s dev %s", addr, dev->name);
        if (!run_cmd_text(cmd)) {
            set_error(error, error_len, "ip address replace failed");
            xgw_tun_close(dev);
            return 0;
        }
    }
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip link set dev %s up", dev->name);
        if (!run_cmd_text(cmd)) {
        set_error(error, error_len, "ip link up failed");
        xgw_tun_close(dev);
        return 0;
        }
    }
    return 1;
}

/* 从 TUN 读取一个报文。 */
int xgw_tun_read(xgw_tun_device_t *dev, uint8_t *buf, size_t buf_cap, size_t *out_len) {
    ssize_t rc;
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (dev == NULL || dev->fd <= 0) {
        return 0;
    }
    rc = read(dev->fd, buf, buf_cap);
    if (rc < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;
        }
        return 0;
    }
    if (out_len != NULL) {
        *out_len = (size_t) rc;
    }
    return 1;
}

/* 向 TUN 写入一个报文。 */
int xgw_tun_write(xgw_tun_device_t *dev, const uint8_t *buf, size_t buf_len) {
    ssize_t rc;
    if (dev == NULL || dev->fd <= 0) {
        return 0;
    }
    rc = write(dev->fd, buf, buf_len);
    return rc == (ssize_t) buf_len;
}

/* 关闭 TUN 文件描述符。 */
void xgw_tun_close(xgw_tun_device_t *dev) {
    if (dev != NULL && dev->fd > 0) {
        close(dev->fd);
        dev->fd = 0;
    }
}

#else
typedef int xgw_tun_linux_translation_unit_anchor;
#endif
