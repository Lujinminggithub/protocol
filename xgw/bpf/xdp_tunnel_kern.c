#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u16);
} xgw_port_map SEC(".maps");

static __always_inline int parse_udp_port(void *data, void *data_end, __u16 *dst_port) {
    struct ethhdr *eth = data;
    if ((void *) (eth + 1) > data_end) {
        return -1;
    }
    if (bpf_ntohs(eth->h_proto) != ETH_P_IP) {
        return -1;
    }

    struct iphdr *iph = (void *) (eth + 1);
    if ((void *) (iph + 1) > data_end) {
        return -1;
    }
    if (iph->protocol != IPPROTO_UDP) {
        return -1;
    }

    struct udphdr *udph = (void *) iph + iph->ihl * 4;
    if ((void *) (udph + 1) > data_end) {
        return -1;
    }
    *dst_port = bpf_ntohs(udph->dest);
    return 0;
}

SEC("xdp")
int xdp_tunnel_ingress(struct xdp_md *ctx) {
    void *data = (void *) (long) ctx->data;
    void *data_end = (void *) (long) ctx->data_end;
    __u16 dst_port = 0;
    __u32 key = 0;
    __u16 *configured_port = 0;

    if (parse_udp_port(data, data_end, &dst_port) < 0) {
        return XDP_PASS;
    }

    configured_port = bpf_map_lookup_elem(&xgw_port_map, &key);
    if (!configured_port) {
        return XDP_PASS;
    }

    if (dst_port != *configured_port) {
        return XDP_PASS;
    }

    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
