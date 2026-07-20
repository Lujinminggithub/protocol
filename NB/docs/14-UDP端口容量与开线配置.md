# UDP 端口容量与开线配置

## 1. 当前结论

一套 entry、middle、exit 三节点拓扑承载 200 条线路时，entry 的 SOCKS5 UDP relay 端口范围配置为：

```text
20000-21023/UDP
```

该范围包含 1024 个端口。每套拓扑自动使用自身 `entry.host` 作为 UDP 公网地址，每个 UDP association 从上述范围分配一个独占端口。

## 2. 容量计算

当前 TikTok 手机实测每条线路稳定同时建立约 3 个 UDP association：

```text
理论下限 = 200 条线路 × 3 个 association = 600 个端口
```

生产环境需要覆盖 association 重建时的新旧连接短暂重叠。按每条线路 4 个并发 association 设计，并预留约 25% 容量：

```text
工程容量 = 200 × 4 × 1.25 = 1000
```

因此向上取整为 1024 个端口。600 只是在没有重连重叠时的理论下限，不能作为生产配置。

## 3. 配置文件

`tools/lab-hosts.json` 的节点和 entry transport 配置：

```json
{
  "entry": {
    "host": "<entry公网IPv4>"
  },
  "transport": {
    "entry": {
      "udp_port_min": 20000,
      "udp_port_max": 21023
    }
  }
}
```

部署工具默认将 `entry.host` 写入 `NB_SOCKS_UDP_ADVERTISE_IP`。如果 SSH 使用域名或 entry 存在多个公网地址，应在 entry 节点配置中增加 `public_ip`；只有特殊场景才使用 transport 的 `udp_advertise_ip` 覆盖默认值。

部署工具将其转换为 systemd 环境变量：

```text
NB_SOCKS_UDP_ADVERTISE_IP=<当前拓扑的entry公网IPv4>
NB_SOCKS_UDP_PORT_MIN=20000
NB_SOCKS_UDP_PORT_MAX=21023
```

端口按轮转方式分配。已占用端口会被跳过；整个范围耗尽时，entry 返回 SOCKS5 失败并记录 `UDP ASSOCIATE bind fail`，不会退回随机端口。

## 4. 网络放行

广州云安全组和主机防火墙必须同时允许：

```text
入站 UDP 20000-21023
来源 0.0.0.0/0，或实际允许的手机客户端公网地址范围
```

TCP SOCKS 监听端口仍单独放行，例如 `1080/TCP`。广州到香港继续使用内网 QUIC `172.16.31.28:4443`，不需要为每条线路增加内部端口。

## 5. 扩容规则

端口使用率持续超过 70% 时预警，超过 85% 时停止新增线路并扩容。若每条线路实测 P99 并发 association 超过 4，应按以下公式重新计算：

```text
所需端口数 = 线路数 × P99 association 数 × 1.25
```

端口范围应避开 Linux 默认临时端口区间 `32768-60999`，优先从 `10000-29999` 中划分连续区间。单套拓扑继续扩展到 400 条线路时，建议直接使用 2048 个端口，而不是压缩安全余量。

## 6. 验证标准

开通后必须同时满足：

1. SOCKS5 UDP_ASSOCIATE 回复地址为 entry 公网 IP，端口位于固定范围。
2. entry 日志出现 `phone-first-rx`。
3. middle 日志出现 `c2s-first-rx` 和 `s2c-first-rx`。
4. exit 日志出现 `target-first-send` 和 `target-first-rx`。
5. entry 最终出现 `phone-first-send`，确认 UDP 回程到达手机。
6. UDP flow 关闭日志中的上下行包数和字节数均大于零。

只有完成以上六项，才能判定 UDP 媒体面真正通过三跳。

## 7. TikTok 目标媒体端口

`20000-21023/UDP` 是手机连接 entry 时使用的 SOCKS5 UDP relay 监听范围，不能替代 TikTok 服务端目标端口的访问策略。

2026-07-17 手机直播实测命中的 TikTok 裸 IP UDP 目标端口为：

```text
50000/UDP
50001/UDP
50008/UDP
50009/UDP
50020/UDP
50021/UDP
```

这些端口必须同时满足两个条件：

1. fail-closed 白名单显式放行。
2. `nb_tiktok_flow_classify()` 将其归类为 `media/latency/prio=4`。

不得把整个高位 UDP 端口段直接加入白名单。发现新目标端口时，应先由 entry 的 `UDP relay policy drop` 日志确认目标地址、端口和流量特征，再按精确端口扩展并补回归测试。

## 8. UDP ASSOCIATE 控制连接兼容

部分手机代理客户端会在 UDP 探测首轮结束后很快关闭 SOCKS5 UDP ASSOCIATE 的 TCP 控制连接，而跨洲链路上的 UDP 回包此时可能仍在途中。entry 在控制连接关闭后保留默认 5 秒的空闲宽限期：关闭 TCP 控制 fd，但继续保留已经认证的 UDP relay socket、客户端地址和子 flow；宽限期内有 UDP 收发活动时，从最近活动时间重新计算回收时间。

该机制不放宽客户端来源校验和目标白名单。宽限期结束后，entry 统一关闭 relay 及其子 flow。日志使用 `control closed`、`grace expired` 和细分后的 datagram reject 阶段记录生命周期。
