/* Linux AF_XDP 实现：包含 UMEM、ring、BPF/XDP program 与 map 绑定。 */

#define _GNU_SOURCE

#ifdef __linux__

#include "xgw_afxdp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef PF_XDP
#define PF_XDP AF_XDP
#endif

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

typedef struct xgw_ring_view {
    uint32_t *producer;
    uint32_t *consumer;
    uint32_t *flags;
    uint8_t *desc_base;
} xgw_ring_view_t;

typedef struct xgw_desc_rx {
    uint64_t addr;
    uint32_t len;
    uint32_t options;
} xgw_desc_rx_t;

typedef struct xgw_desc_tx {
    uint64_t addr;
    uint32_t len;
    uint32_t options;
} xgw_desc_tx_t;

static void xgw_remove_pinned_file(const char *path) {
    if (path != NULL && path[0] != '\0') {
        unlink(path);
    }
}

static void set_error(char *error, size_t error_len, const char *text) {
    if (error_len > 0U) {
        snprintf(error, error_len, "%s", text == NULL ? "" : text);
    }
}

static int run_cmd(char *error, size_t error_len, const char *cmd) {
    int rc;
    rc = system(cmd);
    if (rc != 0) {
        set_error(error, error_len, cmd);
        return 0;
    }
    return 1;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path, 0755) == 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int init_paths(xgw_afxdp_socket_t *sock, char *error, size_t error_len) {
    if (!getcwd(sock->pin_root, sizeof(sock->pin_root))) {
        set_error(error, error_len, "getcwd failed");
        return 0;
    }
    snprintf(sock->bpf_src, sizeof(sock->bpf_src), "%s/bpf/xdp_tunnel_kern.c", sock->pin_root);
    snprintf(sock->bpf_obj, sizeof(sock->bpf_obj), "%s/bpf/xdp_tunnel_kern.o", sock->pin_root);
    snprintf(sock->pin_root, sizeof(sock->pin_root), "/sys/fs/bpf/xgw");
    snprintf(sock->prog_pin, sizeof(sock->prog_pin), "%s/xdp_tunnel_ingress", sock->pin_root);
    snprintf(sock->map_pin, sizeof(sock->map_pin), "%s/xsks_map", sock->pin_root);
    snprintf(sock->config_map_pin, sizeof(sock->config_map_pin), "%s/xgw_port_map", sock->pin_root);
    if (!file_exists(sock->bpf_src)) {
        set_error(error, error_len, "missing bpf/xdp_tunnel_kern.c");
        return 0;
    }
    return 1;
}

static int ensure_bpf_object(xgw_afxdp_socket_t *sock, char *error, size_t error_len) {
    char cmd[1024];
    if (file_exists(sock->bpf_obj)) {
        return 1;
    }
    snprintf(cmd,
             sizeof(cmd),
             "clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -c %s -o %s",
             sock->bpf_src,
             sock->bpf_obj);
    return run_cmd(error, error_len, cmd);
}

static int load_xdp_program(xgw_afxdp_socket_t *sock, char *error, size_t error_len) {
    char cmd[1024];
    if (!ensure_dir(sock->pin_root)) {
        set_error(error, error_len, "cannot create /sys/fs/bpf/xgw");
        return 0;
    }
    xgw_remove_pinned_file(sock->prog_pin);
    xgw_remove_pinned_file(sock->map_pin);
    xgw_remove_pinned_file(sock->config_map_pin);
    snprintf(cmd,
             sizeof(cmd),
             "bpftool prog loadall %s %s type xdp pinmaps %s",
             sock->bpf_obj,
             sock->pin_root,
             sock->pin_root);
    if (!run_cmd(error, error_len, cmd)) {
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "ip link set dev %s xdp pinned %s", sock->device, sock->prog_pin);
    return run_cmd(error, error_len, cmd);
}

static int update_port_map(xgw_afxdp_socket_t *sock, char *error, size_t error_len) {
    char key_hex[9];
    char value_hex[5];
    char cmd[1024];
    uint16_t port_le = sock->port;
    snprintf(key_hex, sizeof(key_hex), "%08x", 0U);
    snprintf(value_hex, sizeof(value_hex), "%04x", port_le);
    snprintf(cmd,
             sizeof(cmd),
             "bpftool map update pinned %s key hex %c%c %c%c %c%c %c%c value hex %c%c %c%c",
             sock->config_map_pin,
             key_hex[0], key_hex[1], key_hex[2], key_hex[3], key_hex[4], key_hex[5], key_hex[6], key_hex[7],
             value_hex[0], value_hex[1], value_hex[2], value_hex[3]);
    return run_cmd(error, error_len, cmd);
}

static int update_xsk_map(xgw_afxdp_socket_t *sock, char *error, size_t error_len) {
    char key_hex[9];
    char value_hex[9];
    char cmd[1024];
    snprintf(key_hex, sizeof(key_hex), "%08x", sock->queue_id);
    snprintf(value_hex, sizeof(value_hex), "%08x", sock->fd);
    snprintf(cmd,
             sizeof(cmd),
             "bpftool map update pinned %s key hex %c%c %c%c %c%c %c%c value hex %c%c %c%c %c%c %c%c",
             sock->map_pin,
             key_hex[0], key_hex[1], key_hex[2], key_hex[3], key_hex[4], key_hex[5], key_hex[6], key_hex[7],
             value_hex[0], value_hex[1], value_hex[2], value_hex[3], value_hex[4], value_hex[5], value_hex[6], value_hex[7]);
    return run_cmd(error, error_len, cmd);
}

static size_t ring_map_len(uint32_t ring_size, uint64_t desc_off, size_t desc_size) {
    return (size_t) desc_off + ((size_t) ring_size * desc_size);
}

static xgw_ring_view_t build_view(void *base, const struct xdp_ring_offset *off) {
    uintptr_t ptr = (uintptr_t) base;
    xgw_ring_view_t view;
    view.producer = (uint32_t *) (ptr + off->producer);
    view.consumer = (uint32_t *) (ptr + off->consumer);
    view.flags = (uint32_t *) (ptr + off->flags);
    view.desc_base = (uint8_t *) (ptr + off->desc);
    return view;
}

static int push_free(xgw_afxdp_socket_t *sock, uint64_t addr) {
    if (sock->free_count >= sock->free_cap) {
        return 0;
    }
    sock->free_frames[sock->free_count++] = addr;
    return 1;
}

static int pop_free(xgw_afxdp_socket_t *sock, uint64_t *addr) {
    if (sock->free_count == 0U) {
        return 0;
    }
    *addr = sock->free_frames[--sock->free_count];
    return 1;
}

static int refill_fill_ring(xgw_afxdp_socket_t *sock) {
    xgw_ring_view_t view;
    view.producer = (uint32_t *) sock->fill_producer;
    view.consumer = (uint32_t *) sock->fill_consumer;
    view.flags = (uint32_t *) sock->fill_flags;
    view.desc_base = sock->fill_desc_base;
    while (sock->free_count > 0U) {
        uint64_t addr = 0;
        uint32_t producer = *view.producer;
        uint32_t consumer = *view.consumer;
        uint32_t index;
        if (producer - consumer >= sock->fill_ring_size) {
            break;
        }
        if (!pop_free(sock, &addr)) {
            break;
        }
        index = producer & sock->fill_mask;
        memcpy(view.desc_base + ((size_t) index * sizeof(uint64_t)), &addr, sizeof(addr));
        *view.producer = producer + 1U;
    }
    return 1;
}

static void reclaim_completions(xgw_afxdp_socket_t *sock) {
    xgw_ring_view_t view;
    view.producer = (uint32_t *) sock->comp_producer;
    view.consumer = (uint32_t *) sock->comp_consumer;
    view.flags = (uint32_t *) sock->comp_flags;
    view.desc_base = sock->comp_desc_base;
    while (*view.consumer != *view.producer) {
        uint32_t index = (*view.consumer) & sock->comp_mask;
        uint64_t addr = 0;
        memcpy(&addr, view.desc_base + ((size_t) index * sizeof(uint64_t)), sizeof(addr));
        push_free(sock, addr);
        *view.consumer = *view.consumer + 1U;
    }
}

static int register_umem(xgw_afxdp_socket_t *sock) {
    struct xdp_umem_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.addr = (uint64_t) (uintptr_t) sock->umem_area;
    reg.len = sock->umem_len;
    reg.chunk_size = sock->frame_size;
    reg.headroom = 0;
    reg.flags = 0;
    return setsockopt(sock->fd, SOL_XDP, XDP_UMEM_REG, &reg, sizeof(reg)) == 0;
}

static int configure_rings(xgw_afxdp_socket_t *sock) {
    int value;
    value = (int) sock->rx_ring_size;
    if (setsockopt(sock->fd, SOL_XDP, XDP_RX_RING, &value, sizeof(value)) != 0) {
        return 0;
    }
    value = (int) sock->tx_ring_size;
    if (setsockopt(sock->fd, SOL_XDP, XDP_TX_RING, &value, sizeof(value)) != 0) {
        return 0;
    }
    value = (int) sock->fill_ring_size;
    if (setsockopt(sock->fd, SOL_XDP, XDP_UMEM_FILL_RING, &value, sizeof(value)) != 0) {
        return 0;
    }
    value = (int) sock->comp_ring_size;
    return setsockopt(sock->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &value, sizeof(value)) == 0;
}

static int mmap_rings(xgw_afxdp_socket_t *sock, char *error, size_t error_len) {
    struct xdp_mmap_offsets offsets;
    socklen_t len = sizeof(offsets);
    xgw_ring_view_t view;

    if (getsockopt(sock->fd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &len) != 0) {
        set_error(error, error_len, "getsockopt XDP_MMAP_OFFSETS failed");
        return 0;
    }

    sock->rx_map_len = ring_map_len(sock->rx_ring_size, offsets.rx.desc, sizeof(struct xdp_desc));
    sock->tx_map_len = ring_map_len(sock->tx_ring_size, offsets.tx.desc, sizeof(struct xdp_desc));
    sock->fill_map_len = ring_map_len(sock->fill_ring_size, offsets.fr.desc, sizeof(uint64_t));
    sock->comp_map_len = ring_map_len(sock->comp_ring_size, offsets.cr.desc, sizeof(uint64_t));

    sock->rx_map = mmap(NULL, sock->rx_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, sock->fd, XDP_PGOFF_RX_RING);
    sock->tx_map = mmap(NULL, sock->tx_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, sock->fd, XDP_PGOFF_TX_RING);
    sock->fill_map = mmap(NULL, sock->fill_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, sock->fd, XDP_UMEM_PGOFF_FILL_RING);
    sock->comp_map = mmap(NULL, sock->comp_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, sock->fd, XDP_UMEM_PGOFF_COMPLETION_RING);
    if (sock->rx_map == MAP_FAILED || sock->tx_map == MAP_FAILED || sock->fill_map == MAP_FAILED || sock->comp_map == MAP_FAILED) {
        set_error(error, error_len, "mmap rings failed");
        return 0;
    }

    view = build_view(sock->rx_map, &offsets.rx);
    sock->rx_producer = view.producer;
    sock->rx_consumer = view.consumer;
    sock->rx_flags = view.flags;
    sock->rx_desc_base = view.desc_base;

    view = build_view(sock->tx_map, &offsets.tx);
    sock->tx_producer = view.producer;
    sock->tx_consumer = view.consumer;
    sock->tx_flags = view.flags;
    sock->tx_desc_base = view.desc_base;

    view = build_view(sock->fill_map, &offsets.fr);
    sock->fill_producer = view.producer;
    sock->fill_consumer = view.consumer;
    sock->fill_flags = view.flags;
    sock->fill_desc_base = view.desc_base;

    view = build_view(sock->comp_map, &offsets.cr);
    sock->comp_producer = view.producer;
    sock->comp_consumer = view.consumer;
    sock->comp_flags = view.flags;
    sock->comp_desc_base = view.desc_base;

    return 1;
}

static int bind_socket(xgw_afxdp_socket_t *sock, char *error, size_t error_len) {
    struct sockaddr_xdp sxdp;
    memset(&sxdp, 0, sizeof(sxdp));
    sxdp.sxdp_family = AF_XDP;
    sxdp.sxdp_ifindex = sock->ifindex;
    sxdp.sxdp_queue_id = sock->queue_id;
    sxdp.sxdp_flags = 0;
    if (bind(sock->fd, (struct sockaddr *) &sxdp, sizeof(sxdp)) != 0) {
        set_error(error, error_len, "bind AF_XDP socket failed");
        return 0;
    }
    return 1;
}

int xgw_afxdp_open(xgw_afxdp_socket_t *sock, const char *device, uint32_t queue_id, uint16_t port, char *error, size_t error_len) {
    memset(sock, 0, sizeof(*sock));
    snprintf(sock->device, sizeof(sock->device), "%s", device == NULL ? "" : device);
    sock->queue_id = queue_id;
    sock->port = port;
    sock->fd = -1;
    sock->frame_size = 4096U;
    sock->frame_count = 4096U;
    sock->rx_ring_size = 1024U;
    sock->tx_ring_size = 1024U;
    sock->fill_ring_size = 2048U;
    sock->comp_ring_size = 1024U;
    sock->rx_mask = sock->rx_ring_size - 1U;
    sock->tx_mask = sock->tx_ring_size - 1U;
    sock->fill_mask = sock->fill_ring_size - 1U;
    sock->comp_mask = sock->comp_ring_size - 1U;

    if (!init_paths(sock, error, error_len)) {
        return 0;
    }
    if (!ensure_bpf_object(sock, error, error_len)) {
        return 0;
    }
    sock->ifindex = if_nametoindex(sock->device);
    if (sock->ifindex == 0U) {
        set_error(error, error_len, "if_nametoindex failed");
        return 0;
    }
    sock->fd = socket(PF_XDP, SOCK_RAW, 0);
    if (sock->fd < 0) {
        set_error(error, error_len, "socket(AF_XDP) failed");
        return 0;
    }
    sock->umem_len = (size_t) sock->frame_size * (size_t) sock->frame_count;
    sock->umem_area = mmap(NULL, sock->umem_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sock->umem_area == MAP_FAILED) {
        set_error(error, error_len, "mmap umem failed");
        xgw_afxdp_close(sock);
        return 0;
    }
    sock->free_cap = sock->frame_count;
    sock->free_frames = (uint64_t *) calloc(sock->free_cap, sizeof(uint64_t));
    if (sock->free_frames == NULL) {
        set_error(error, error_len, "calloc free frame list failed");
        xgw_afxdp_close(sock);
        return 0;
    }
    if (!register_umem(sock)) {
        set_error(error, error_len, "setsockopt XDP_UMEM_REG failed");
        xgw_afxdp_close(sock);
        return 0;
    }
    if (!configure_rings(sock)) {
        set_error(error, error_len, "configure AF_XDP rings failed");
        xgw_afxdp_close(sock);
        return 0;
    }
    if (!mmap_rings(sock, error, error_len)) {
        xgw_afxdp_close(sock);
        return 0;
    }
    if (!bind_socket(sock, error, error_len)) {
        xgw_afxdp_close(sock);
        return 0;
    }
    if (!load_xdp_program(sock, error, error_len)) {
        xgw_afxdp_close(sock);
        return 0;
    }
    if (!update_xsk_map(sock, error, error_len)) {
        xgw_afxdp_close(sock);
        return 0;
    }
    if (!update_port_map(sock, error, error_len)) {
        xgw_afxdp_close(sock);
        return 0;
    }
    {
        uint32_t i;
        for (i = 0; i < sock->frame_count; ++i) {
        push_free(sock, (uint64_t) i * (uint64_t) sock->frame_size);
        }
    }
    refill_fill_ring(sock);
    sock->ready = 1;
    return 1;
}

/* 从 AF_XDP RX ring 提取一个包。 */
int xgw_afxdp_recv(xgw_afxdp_socket_t *sock, uint8_t *buf, size_t buf_cap, size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (sock == NULL || !sock->ready) {
        return 0;
    }
    reclaim_completions(sock);
    if (*(uint32_t *) sock->rx_producer == *(uint32_t *) sock->rx_consumer) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = sock->fd;
        pfd.events = POLLIN;
        (void) poll(&pfd, 1, 10);
        if (*(uint32_t *) sock->rx_producer == *(uint32_t *) sock->rx_consumer) {
            return 1;
        }
    }
    {
        uint32_t consumer = *(uint32_t *) sock->rx_consumer;
        uint32_t index = consumer & sock->rx_mask;
        xgw_desc_rx_t desc;
        memcpy(&desc, sock->rx_desc_base + ((size_t) index * sizeof(struct xdp_desc)), sizeof(desc));
        *(uint32_t *) sock->rx_consumer = consumer + 1U;
        if (desc.len > buf_cap || desc.addr + desc.len > sock->umem_len) {
            push_free(sock, desc.addr);
            refill_fill_ring(sock);
            return 0;
        }
        memcpy(buf, sock->umem_area + desc.addr, desc.len);
        if (out_len != NULL) {
            *out_len = desc.len;
        }
        push_free(sock, desc.addr);
        refill_fill_ring(sock);
    }
    return 1;
}

/* 向 AF_XDP TX ring 放入一个包。 */
int xgw_afxdp_send(xgw_afxdp_socket_t *sock, const uint8_t *buf, size_t buf_len) {
    uint64_t addr = 0;
    uint32_t producer;
    uint32_t index;
    xgw_desc_tx_t desc;
    if (sock == NULL || !sock->ready || buf_len == 0U || buf_len > sock->frame_size) {
        return 0;
    }
    reclaim_completions(sock);
    if (!pop_free(sock, &addr)) {
        return 0;
    }
    memcpy(sock->umem_area + addr, buf, buf_len);
    producer = *(uint32_t *) sock->tx_producer;
    index = producer & sock->tx_mask;
    memset(&desc, 0, sizeof(desc));
    desc.addr = addr;
    desc.len = (uint32_t) buf_len;
    memcpy(sock->tx_desc_base + ((size_t) index * sizeof(struct xdp_desc)), &desc, sizeof(desc));
    *(uint32_t *) sock->tx_producer = producer + 1U;
    if (sendto(sock->fd, NULL, 0, MSG_DONTWAIT, NULL, 0) < 0 && errno != EBUSY && errno != EAGAIN) {
        push_free(sock, addr);
        return 0;
    }
    return 1;
}

/* 关闭 AF_XDP 资源并卸载已绑定的 XDP 程序。 */
void xgw_afxdp_close(xgw_afxdp_socket_t *sock) {
    char cmd[512];
    if (sock == NULL) {
        return;
    }
    if (sock->device[0] != '\0' && sock->prog_pin[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "ip link set dev %s xdp off >/dev/null 2>&1", sock->device);
        (void) system(cmd);
    }
    if (sock->rx_map && sock->rx_map != MAP_FAILED) {
        munmap(sock->rx_map, sock->rx_map_len);
    }
    if (sock->tx_map && sock->tx_map != MAP_FAILED) {
        munmap(sock->tx_map, sock->tx_map_len);
    }
    if (sock->fill_map && sock->fill_map != MAP_FAILED) {
        munmap(sock->fill_map, sock->fill_map_len);
    }
    if (sock->comp_map && sock->comp_map != MAP_FAILED) {
        munmap(sock->comp_map, sock->comp_map_len);
    }
    if (sock->umem_area && sock->umem_area != MAP_FAILED) {
        munmap(sock->umem_area, sock->umem_len);
    }
    if (sock->fd >= 0) {
        close(sock->fd);
    }
    free(sock->free_frames);
    memset(sock, 0, sizeof(*sock));
    sock->fd = -1;
}

#else
typedef int xgw_afxdp_linux_translation_unit_anchor;
#endif
