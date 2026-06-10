/* 运行时主循环：把 TUN、UDP/AF_XDP 与数据面逻辑串接起来。 */

#define _POSIX_C_SOURCE 200809L

#include "xgw_runtime.h"

#include "xgw_afxdp.h"
#include "xgw_dataplane.h"
#include "xgw_obfs.h"
#include "xgw_outbound.h"
#include "xgw_transport.h"
#include "xgw_tun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void xgw_sleep_ms(unsigned int ms) { Sleep(ms); }
#else
#include <time.h>
static void xgw_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (long) (ms % 1000U) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

typedef struct xgw_node_hops {
    const xgw_endpoint_t *current;
    const xgw_endpoint_t *previous;
    const xgw_endpoint_t *next;
} xgw_node_hops_t;

static void set_error(char *error, size_t error_len, const char *text) {
    if (error_len > 0U) {
        snprintf(error, error_len, "%s", text == NULL ? "" : text);
    }
}

static int split_host_port(const char *address, char *host, size_t host_len, uint16_t *port) {
    const char *colon = strrchr(address, ':');
    unsigned long parsed_port;
    char *end = NULL;
    if (colon == NULL || colon == address) {
        return 0;
    }
    snprintf(host, host_len, "%.*s", (int) (colon - address), address);
    parsed_port = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || parsed_port > 65535UL) {
        return 0;
    }
    *port = (uint16_t) parsed_port;
    return 1;
}

/* 从路径里解析当前节点、上游和下游 hop。 */
static int resolve_hops(const xgw_runtime_config_t *config, xgw_node_hops_t *hops) {
    size_t i;
    memset(hops, 0, sizeof(*hops));
    for (i = 0; i < config->path.hop_count; ++i) {
        const xgw_endpoint_t *hop = &config->path.hops[i];
        if ((config->hop_name[0] != '\0' && strcmp(hop->name, config->hop_name) == 0) ||
            (config->hop_name[0] == '\0' && hop->role == config->role && hops->current == NULL)) {
            hops->current = hop;
            if (i > 0U) {
                hops->previous = &config->path.hops[i - 1U];
            }
            if (i + 1U < config->path.hop_count) {
                hops->next = &config->path.hops[i + 1U];
            }
            return 1;
        }
    }
    return 0;
}

/* 判断当前角色是否还需要继续向下游转发。 */
static int is_forwarder(xgw_role_t role) {
    return role == XGW_ROLE_INGRESS || role == XGW_ROLE_RELAY;
}

static int obfs_hop_mode(const xgw_runtime_config_t *config) {
    return strcmp(config->obfs_scope, "hop") == 0 || config->obfs_scope[0] == '\0';
}

static const xgw_pool_node_t *select_pool_node(const xgw_dataplane_t *dp) {
    return xgw_pool_select_best(&dp->pool);
}

static unsigned int pacing_delay_ms(const xgw_profile_t *profile, size_t bytes) {
    double seconds;
    double millis;
    unsigned int floor_delay;
    if (profile == NULL || profile->pacing_rate_bps == 0ULL || bytes == 0U) {
        return 0U;
    }
    seconds = ((double) bytes * 8.0) / (double) profile->pacing_rate_bps;
    millis = seconds * 1000.0;
    floor_delay = (unsigned int) millis;
    if (profile->pacing_interval_us > 0U) {
        unsigned int min_ms = (profile->pacing_interval_us + 999U) / 1000U;
        if (floor_delay < min_ms) {
            floor_delay = min_ms;
        }
    }
    return floor_delay;
}

static void maybe_pace(const xgw_profile_t *profile, size_t bytes) {
    unsigned int delay = pacing_delay_ms(profile, bytes);
    if (delay > 0U) {
        xgw_sleep_ms(delay);
    }
}

/* 通过 AF_XDP 发送一批数据帧，可按 profile 执行冗余副本和 pacing。 */
static int send_batch_afxdp(xgw_afxdp_socket_t *xsk, const xgw_profile_t *profile, const xgw_frame_batch_t *batch, const uint8_t *fec_frame, size_t fec_len) {
    size_t i;
    uint32_t copies;
    uint32_t copy_index;
    copies = profile != NULL && profile->redundant_copies > 0U ? profile->redundant_copies : 1U;
    for (i = 0; i < batch->frame_count; ++i) {
        for (copy_index = 0; copy_index < copies; ++copy_index) {
            if (!xgw_afxdp_send(xsk, batch->frames[i], batch->frame_lengths[i])) {
                return 0;
            }
            maybe_pace(profile, batch->frame_lengths[i]);
        }
    }
    if (fec_len > 0U) {
        for (copy_index = 0; copy_index < copies; ++copy_index) {
            if (!xgw_afxdp_send(xsk, fec_frame, fec_len)) {
                return 0;
            }
            maybe_pace(profile, fec_len);
        }
    }
    return 1;
}

static int send_batch_udp_target(xgw_udp_socket_t *udp,
                                 const xgw_profile_t *profile,
                                 const xgw_obfs_t *obfs,
                                 int hop_obfs,
                                 const char *host,
                                 uint16_t port,
                                 const xgw_frame_batch_t *batch,
                                 const uint8_t *fec_frame,
                                 size_t fec_len) {
    size_t i;
    uint32_t copies;
    uint32_t copy_index;
    uint8_t obfs_buf[XGW_MAX_FRAME_SIZE + XGW_OBFS_SALT_LEN];
    if (host == NULL || host[0] == '\0' || port == 0U) {
        return 0;
    }
    copies = profile != NULL && profile->redundant_copies > 0U ? profile->redundant_copies : 1U;
    for (i = 0; i < batch->frame_count; ++i) {
        for (copy_index = 0; copy_index < copies; ++copy_index) {
            const uint8_t *payload = batch->frames[i];
            size_t payload_len = batch->frame_lengths[i];
            if (xgw_obfs_enabled(obfs) && hop_obfs) {
                payload_len = xgw_obfs_encode(obfs, batch->frames[i], batch->frame_lengths[i], obfs_buf, sizeof(obfs_buf));
                payload = obfs_buf;
            }
            if (!xgw_udp_send(udp, payload, payload_len, host, port)) {
                return 0;
            }
            maybe_pace(profile, batch->frame_lengths[i]);
        }
    }
    if (fec_len > 0U) {
        for (copy_index = 0; copy_index < copies; ++copy_index) {
            const uint8_t *payload = fec_frame;
            size_t payload_len = fec_len;
            if (xgw_obfs_enabled(obfs) && hop_obfs) {
                payload_len = xgw_obfs_encode(obfs, fec_frame, fec_len, obfs_buf, sizeof(obfs_buf));
                payload = obfs_buf;
            }
            if (!xgw_udp_send(udp, payload, payload_len, host, port)) {
                return 0;
            }
            maybe_pace(profile, fec_len);
        }
    }
    return 1;
}

static int send_batch_outbound(xgw_outbound_t *outbound,
                               const xgw_profile_t *profile,
                               const xgw_obfs_t *obfs,
                               int hop_obfs,
                               const xgw_frame_batch_t *batch,
                               const uint8_t *fec_frame,
                               size_t fec_len) {
    size_t i;
    uint32_t copies;
    uint32_t copy_index;
    uint8_t obfs_buf[XGW_MAX_FRAME_SIZE + XGW_OBFS_SALT_LEN];
    copies = profile != NULL && profile->redundant_copies > 0U ? profile->redundant_copies : 1U;
    for (i = 0; i < batch->frame_count; ++i) {
        for (copy_index = 0; copy_index < copies; ++copy_index) {
            const uint8_t *payload = batch->frames[i];
            size_t payload_len = batch->frame_lengths[i];
            if (xgw_obfs_enabled(obfs) && hop_obfs) {
                payload_len = xgw_obfs_encode(obfs, batch->frames[i], batch->frame_lengths[i], obfs_buf, sizeof(obfs_buf));
                payload = obfs_buf;
            }
            if (!xgw_outbound_send(outbound, payload, payload_len, outbound->current_target_host, outbound->current_target_port)) {
                return 0;
            }
            maybe_pace(profile, batch->frame_lengths[i]);
        }
    }
    if (fec_len > 0U) {
        for (copy_index = 0; copy_index < copies; ++copy_index) {
            const uint8_t *payload = fec_frame;
            size_t payload_len = fec_len;
            if (xgw_obfs_enabled(obfs) && hop_obfs) {
                payload_len = xgw_obfs_encode(obfs, fec_frame, fec_len, obfs_buf, sizeof(obfs_buf));
                payload = obfs_buf;
            }
            if (!xgw_outbound_send(outbound, payload, payload_len, outbound->current_target_host, outbound->current_target_port)) {
                return 0;
            }
            maybe_pace(profile, fec_len);
        }
    }
    return 1;
}

/* 运行一个节点的主循环。 */
int xgw_runtime_run(const xgw_runtime_config_t *config, char *error, size_t error_len) {
    xgw_node_hops_t hops;
    xgw_dataplane_t *dp = NULL;
    xgw_tun_device_t tun;
    xgw_udp_socket_t udp;
    xgw_afxdp_socket_t xsk;
    xgw_outbound_t outbound;
    xgw_obfs_t obfs;
    xgw_session_t *session;
    xgw_frame_batch_t *batch = NULL;
    xgw_reassembly_result_t *result = NULL;
    uint8_t *fec_frame = NULL;
    uint8_t *tun_buf = NULL;
    size_t tun_len = 0;
    size_t fec_len = 0;
    xgw_packet_t *packet = NULL;
    int tun_opened = 0;
    int net_opened = 0;
    int hop_obfs = 0;
    char host[64];
    uint16_t port = 0;

    memset(&tun, 0, sizeof(tun));
    memset(&udp, 0, sizeof(udp));
    memset(&xsk, 0, sizeof(xsk));
    memset(&outbound, 0, sizeof(outbound));
    xgw_obfs_init(&obfs, config->obfs_mode, config->obfs_key);
    hop_obfs = xgw_obfs_enabled(&obfs) && obfs_hop_mode(config);
    if (!resolve_hops(config, &hops)) {
        set_error(error, error_len, "cannot resolve current hop from path");
        return 0;
    }
    if (!split_host_port(hops.current->address, host, sizeof(host), &port)) {
        set_error(error, error_len, "invalid current hop address");
        return 0;
    }
    dp = (xgw_dataplane_t *) calloc(1, sizeof(*dp));
    batch = (xgw_frame_batch_t *) calloc(1, sizeof(*batch));
    result = (xgw_reassembly_result_t *) calloc(1, sizeof(*result));
    fec_frame = (uint8_t *) calloc(1, XGW_MAX_FRAME_SIZE);
    tun_buf = (uint8_t *) calloc(1, 65535U);
    packet = (xgw_packet_t *) calloc(1, sizeof(*packet));
    if (dp == NULL || batch == NULL || result == NULL || fec_frame == NULL || tun_buf == NULL || packet == NULL) {
        set_error(error, error_len, "runtime allocation failed");
        free(dp);
        free(batch);
        free(result);
        free(fec_frame);
        free(tun_buf);
        free(packet);
        return 0;
    }
    xgw_dataplane_init(dp, config);
    session = xgw_session_upsert(&dp->sessions, 1U, config->allow_policy.group_id, config->tun_addr, "", 0U, time(NULL));
    if (session == NULL) {
        set_error(error, error_len, "cannot allocate outbound session");
        free(dp);
        free(batch);
        free(result);
        free(fec_frame);
        free(tun_buf);
        free(packet);
        return 0;
    }
    if (config->tun_name[0] != '\0' && config->tun_addr[0] != '\0') {
        if (!xgw_tun_open(&tun, config->tun_name, config->tun_addr, config->profile.mtu, error, error_len)) {
            free(dp);
            free(batch);
            free(result);
            free(fec_frame);
            free(tun_buf);
            free(packet);
            return 0;
        }
        tun_opened = 1;
    }
    if (strcmp(config->transport, "afxdp") == 0) {
        if (!xgw_afxdp_open(&xsk, config->device, config->queue_id, port, error, error_len)) {
            if (tun_opened) {
                xgw_tun_close(&tun);
            }
            free(dp);
            free(batch);
            free(result);
            free(fec_frame);
            free(tun_buf);
            free(packet);
            return 0;
        }
        net_opened = 1;
    } else {
        if (!xgw_udp_listen(&udp, config->listen_host, port)) {
            if (tun_opened) {
                xgw_tun_close(&tun);
            }
            set_error(error, error_len, "cannot open udp socket");
            free(dp);
            free(batch);
            free(result);
            free(fec_frame);
            free(tun_buf);
            free(packet);
            return 0;
        }
        if (!xgw_udp_set_buffers(&udp,
                                 (int) config->tuning.udp_rcvbuf_bytes,
                                 (int) config->tuning.udp_sndbuf_bytes)) {
            printf("runtime.warn udp_set_buffers_failed rcv=%u snd=%u\n",
                   config->tuning.udp_rcvbuf_bytes,
                   config->tuning.udp_sndbuf_bytes);
            fflush(stdout);
        }
        net_opened = 1;
    }

    printf("runtime.start role=%s transport=%s proxy_mode=%s acl_mode=%s current=%s prev=%s next=%s\n",
           xgw_role_name(config->role),
           config->transport,
           config->proxy_mode,
           config->acl_mode,
           hops.current == NULL ? "" : hops.current->address,
           hops.previous == NULL ? "" : hops.previous->address,
           hops.next == NULL ? "" : hops.next->address);
    printf("runtime.negotiation auth=%s udp=%d congestion=%s bbr_profile=%s rx_bps=%llu tx_bps=%llu windows=%u/%u/%u/%u idle=%u keepalive=%u pmtud_disabled=%d outbound=%s:%u\n",
           config->auth_token[0] != '\0' ? "set" : "unset",
           config->enable_udp,
           xgw_congestion_mode_name(config->congestion_mode),
           xgw_bbr_profile_name(config->bbr_profile),
           (unsigned long long) config->advertised_rx_bps,
           (unsigned long long) config->advertised_tx_bps,
           config->initial_stream_receive_window,
           config->max_stream_receive_window,
           config->initial_connection_receive_window,
           config->max_connection_receive_window,
           config->max_idle_timeout_sec,
           config->keepalive_sec,
           config->disable_path_mtu_discovery,
           config->outbound_host,
           config->outbound_port);
    fflush(stdout);

    if (config->outbound_host[0] != '\0' && config->outbound_port > 0U) {
        xgw_outbound_type_t outbound_type = XGW_OUTBOUND_DIRECT;
        if (!xgw_outbound_parse_type(config->outbound_type, &outbound_type)) {
            set_error(error, error_len, "invalid outbound_type");
            goto cleanup;
        }
        if (!xgw_outbound_open(&outbound,
                               outbound_type,
                               "default",
                               config->outbound_host,
                               config->outbound_port,
                               config->outbound_username,
                               config->outbound_password,
                               error,
                               error_len)) {
            goto cleanup;
        }
        printf("runtime.outbound selected name=default type=%s host=%s port=%u\n",
               xgw_outbound_type_name(outbound_type),
               config->outbound_host,
               config->outbound_port);
        fflush(stdout);
        snprintf(outbound.current_target_host, sizeof(outbound.current_target_host), "%s", config->outbound_host);
        outbound.current_target_port = config->outbound_port;
    } else if (strcmp(config->proxy_mode, "fixed-path") != 0) {
        const xgw_pool_node_t *pool_node = select_pool_node(dp);
        if (pool_node != NULL) {
            xgw_outbound_type_t outbound_type = XGW_OUTBOUND_DIRECT;
            if (!xgw_outbound_parse_type(pool_node->outbound_type, &outbound_type)) {
                set_error(error, error_len, "invalid pool outbound type");
                goto cleanup;
            }
            if (!xgw_outbound_open(&outbound,
                                   outbound_type,
                                   pool_node->name,
                                   pool_node->host,
                                   pool_node->port,
                                   pool_node->username,
                                   pool_node->password,
                                   error,
                                   error_len)) {
                goto cleanup;
            }
            snprintf(outbound.current_target_host, sizeof(outbound.current_target_host), "%s", pool_node->host);
            outbound.current_target_port = pool_node->port;
            printf("runtime.outbound selected pool=%s type=%s host=%s port=%u\n",
                   pool_node->name,
                   pool_node->outbound_type,
                   pool_node->host,
                   pool_node->port);
            fflush(stdout);
        }
    }

    while (1) {
        int did_work = 0;
        if (tun_opened && hops.next != NULL) {
            if (xgw_tun_read(&tun, tun_buf, 65535U, &tun_len) && tun_len > 0U) {
                if (!xgw_dataplane_build_outbound(dp, session, tun_buf, tun_len, batch, fec_frame, XGW_MAX_FRAME_SIZE, &fec_len)) {
                    set_error(error, error_len, xgw_dataplane_last_error());
                    break;
                }
                if (strcmp(config->transport, "afxdp") == 0) {
                    if (!send_batch_afxdp(&xsk, &config->profile, batch, fec_frame, fec_len)) {
                        set_error(error, error_len, "af_xdp send failed");
                        break;
                    }
                } else {
                    const xgw_pool_node_t *pool_node = select_pool_node(dp);
                    if (pool_node != NULL && strcmp(config->proxy_mode, "fixed-path") != 0) {
                        if (!send_batch_outbound(&outbound, &config->profile, &obfs, hop_obfs, batch, fec_frame, fec_len)) {
                            set_error(error, error_len, "udp send failed");
                            break;
                        }
                        printf("runtime.route pool=%s type=%s host=%s port=%u\n",
                               pool_node->name,
                               pool_node->outbound_type,
                               pool_node->host,
                               pool_node->port);
                        fflush(stdout);
                    } else {
                        char next_host[64];
                        uint16_t next_port = 0;
                        if (hops.next == NULL || !split_host_port(hops.next->address, next_host, sizeof(next_host), &next_port)) {
                            set_error(error, error_len, "invalid next hop address");
                            break;
                        }
                        if (!send_batch_udp_target(&udp, &config->profile, &obfs, hop_obfs, next_host, next_port, batch, fec_frame, fec_len)) {
                            set_error(error, error_len, "udp send failed");
                            break;
                        }
                    }
                }
                did_work = 1;
            }
        }

        memset(packet, 0, sizeof(*packet));
        if (strcmp(config->transport, "afxdp") == 0) {
            size_t rx_len = 0;
            if (xgw_afxdp_recv(&xsk, packet->data, sizeof(packet->data), &rx_len)) {
                packet->data_len = rx_len;
                did_work = 1;
            }
        } else {
            int recv_rc = xgw_udp_recv(&udp, packet, 5);
            if (recv_rc < 0) {
                set_error(error, error_len, "udp receive failed");
                break;
            }
            if (recv_rc > 0) {
                if (hop_obfs) {
                    uint8_t plain[65535];
                    size_t plain_len = xgw_obfs_decode(&obfs, packet->data, packet->data_len, plain, sizeof(plain));
                    if (plain_len > 0U) {
                        memcpy(packet->data, plain, plain_len);
                        packet->data_len = plain_len;
                    }
                }
                did_work = 1;
            }
        }

        if (packet->data_len > 0U) {
            char summary[256];
            printf("runtime.recv len=%zu remote=%s:%u\n",
                   packet->data_len,
                   packet->remote_host[0] == '\0' ? "-" : packet->remote_host,
                   packet->remote_port);
            fflush(stdout);
            memset(result, 0, sizeof(*result));
            xgw_dataplane_process_frame(dp, packet, result, summary, sizeof(summary));
            printf("runtime.proc %s\n", summary);
            fflush(stdout);
            if (result->packet_len > 0U) {
                if (is_forwarder(config->role) && hops.next != NULL) {
                    if (!xgw_dataplane_build_outbound(dp, session, result->packet, result->packet_len, batch, fec_frame, XGW_MAX_FRAME_SIZE, &fec_len)) {
                        set_error(error, error_len, xgw_dataplane_last_error());
                        break;
                    }
                    if (strcmp(config->transport, "afxdp") == 0) {
                        if (!send_batch_afxdp(&xsk, &config->profile, batch, fec_frame, fec_len)) {
                            set_error(error, error_len, "af_xdp forward failed");
                            break;
                        }
                    } else {
                        const xgw_pool_node_t *pool_node = select_pool_node(dp);
                        if (pool_node != NULL && strcmp(config->proxy_mode, "fixed-path") != 0) {
                            if (!send_batch_outbound(&outbound, &config->profile, &obfs, hop_obfs, batch, fec_frame, fec_len)) {
                                set_error(error, error_len, "udp forward failed");
                                break;
                            }
                            printf("runtime.forward pool=%s type=%s host=%s port=%u\n",
                                   pool_node->name,
                                   pool_node->outbound_type,
                                   pool_node->host,
                                   pool_node->port);
                            fflush(stdout);
                        } else {
                            char next_host[64];
                            uint16_t next_port = 0;
                            if (hops.next == NULL || !split_host_port(hops.next->address, next_host, sizeof(next_host), &next_port)) {
                                set_error(error, error_len, "invalid next hop address");
                                break;
                            }
                            if (!send_batch_udp_target(&udp, &config->profile, &obfs, hop_obfs, next_host, next_port, batch, fec_frame, fec_len)) {
                                set_error(error, error_len, "udp forward failed");
                                break;
                            }
                        }
                    }
                } else if (tun_opened) {
                    if (!xgw_tun_write(&tun, result->packet, result->packet_len)) {
                        set_error(error, error_len, "tun write failed");
                        break;
                    }
                }
            }
        }

        if (!did_work) {
            xgw_sleep_ms(1U);
        }
    }

    if (net_opened) {
        if (strcmp(config->transport, "afxdp") == 0) {
            xgw_afxdp_close(&xsk);
        } else {
            xgw_udp_close(&udp);
        }
    }
    if (tun_opened) {
        xgw_tun_close(&tun);
    }
cleanup:
    xgw_outbound_close(&outbound);
    free(dp);
    free(batch);
    free(result);
    free(fec_frame);
    free(tun_buf);
    free(packet);
    return 0;
}
