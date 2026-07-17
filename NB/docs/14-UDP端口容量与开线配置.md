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
