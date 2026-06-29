#ifndef XGW_BRIDGE_H
#define XGW_BRIDGE_H

/* 本地桥接面：让上层代理前端把 TCP 流量直接交给 xgw 后端核心。 */

#include "xgw_config.h"

#include <stddef.h>
#include <stdint.h>

typedef struct xgw_bridge_server xgw_bridge_server_t;
typedef struct xgw_bridge_egress xgw_bridge_egress_t;

/* 启动本地 TCP bridge 监听，供 HY2 前端接入。 */
int xgw_bridge_server_start(xgw_bridge_server_t **out_server,
                            const xgw_runtime_config_t *config,
                            char *error,
                            size_t error_len);
/* 轮询处理 bridge 监听上的新连接。 */
int xgw_bridge_server_poll(xgw_bridge_server_t *server, int timeout_ms);
/* 取出一条待发送到 xgw 链路的桥接载荷。返回 1=有数据，0=没有数据，-1=错误。 */
int xgw_bridge_server_dequeue(xgw_bridge_server_t *server,
                              uint8_t *buf,
                              size_t buf_cap,
                              size_t *out_len,
                              char *error,
                              size_t error_len);
/* 处理一条从 xgw 链路收到的桥接载荷。返回 1=已消费，0=不是桥接载荷，-1=错误。 */
int xgw_bridge_server_handle_payload(xgw_bridge_server_t *server,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     char *error,
                                     size_t error_len);
/* 停止 bridge 监听并释放资源。 */
void xgw_bridge_server_stop(xgw_bridge_server_t *server);

/* 初始化 egress 侧 TCP 会话桥。 */
int xgw_bridge_egress_init(xgw_bridge_egress_t **out_bridge, char *error, size_t error_len);
/* 消费一条桥接载荷并在 egress 侧执行 TCP 连接/收发。 */
int xgw_bridge_egress_handle_payload(xgw_bridge_egress_t *bridge,
                                     const char *upstream_host,
                                     uint16_t upstream_port,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     char *error,
                                     size_t error_len);
/* 读取当前 egress 记录的真实上游地址。 */
int xgw_bridge_egress_get_upstream(const xgw_bridge_egress_t *bridge,
                                   char *host,
                                   size_t host_len,
                                   uint16_t *port);
/* 主动扫描所有 egress 活跃 stream 的回包。 */
void xgw_bridge_egress_poll_all(xgw_bridge_egress_t *bridge);
/* 返回当前活跃 egress stream 数。 */
size_t xgw_bridge_egress_active_count(const xgw_bridge_egress_t *bridge);
/* 轮询 egress TCP 会话上的回包，并序列化成桥接载荷。 */
int xgw_bridge_egress_dequeue(xgw_bridge_egress_t *bridge,
                              uint8_t *buf,
                              size_t buf_cap,
                              size_t *out_len,
                              char *error,
                              size_t error_len);
int xgw_bridge_egress_dequeue_ex(xgw_bridge_egress_t *bridge,
                                 uint8_t *buf,
                                 size_t buf_cap,
                                 size_t *out_len,
                                 char *upstream_host,
                                 size_t upstream_host_len,
                                 uint16_t *upstream_port,
                                 char *error,
                                 size_t error_len);

/* burst 出队回调：每序列化出一条回程帧即回调一次。
 * 返回 0=继续 burst，<0=请求停止 burst（如下游硬错误）。 */
typedef int (*xgw_bridge_egress_emit_fn)(void *ctx,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         const char *upstream_host,
                                         uint16_t upstream_port);

/* 一次全表扫描后，在预算内连续吐多条回程帧（消除"每 tick 1 chunk"节流）。
 * 同优先级内按 round-robin 公平调度，避免低优先级流饥饿。
 * max_chunks/max_bytes/max_us 任一为 0 表示该维度不限。
 * 返回吐出的帧数（>=0），或 -1=错误。 */
int xgw_bridge_egress_dequeue_burst_ex(xgw_bridge_egress_t *bridge,
                                       size_t max_chunks,
                                       size_t max_bytes,
                                       uint64_t max_us,
                                       xgw_bridge_egress_emit_fn emit,
                                       void *ctx,
                                       char *error,
                                       size_t error_len);
/* 释放 egress 侧桥资源。 */
void xgw_bridge_egress_free(xgw_bridge_egress_t *bridge);

/* 设置 bridge 日志级别（0=安静，1=正常，2+=逐包 verbose）。
 * 默认 1：抑制逐包 deliver/send 日志，避免写满磁盘。 */
void xgw_bridge_set_log_level(uint32_t level);

/* 设置 egress pending 队列软上限（运行时可配，钳到编译期硬上限）。0=用硬上限。 */
void xgw_bridge_set_pending_chunks(uint32_t chunks);

#endif
