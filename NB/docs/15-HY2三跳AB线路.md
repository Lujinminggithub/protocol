# HY2 三跳 A/B 线路

## 拓扑

```text
手机
  -> 广州公网 106.75.169.83:51000/UDP
  -> 广州 DNAT/SNAT
  -> 香港内网 172.16.31.28:51000/UDP（HY2 server）
  -> 香港 127.0.0.1:51001（HY2 client SOCKS5）
  -> KZ 2.135.147.106:51000/UDP（HY2 server）
  -> 公网目标
```

该线路与 NB 共用相同的广州、香港和 KZ 服务器，不修改 NB 进程、端口、拥塞控制或 FEC 配置。用于对比两种传输实现在同一物理链路上的直播稳定性、画质和延迟。

## 服务

- 广州：`hy2-ab-forward.service`
- 香港第一跳服务端：`hy2-ab-server.service`
- 香港到 KZ 客户端：`hy2-ab-client.service`
- KZ 最终服务端：`hy2-ab-server.service`

香港和 KZ 使用 Hysteria `v2.10.0`，二进制已按官方 `hashes.txt` 校验。两跳均使用默认 BBR，不设置人为 Brutal 带宽值。

## 验证结果

- TCP：通过该线路访问 `ip.sb`，出口为 `2.135.147.106`。
- UDP：通过 SOCKS5 UDP ASSOCIATE 发送 DNS 请求并收到响应。
- 广州 DNAT 和 SNAT 规则均有命中计数。
- 香港到 KZ 日志确认 `udpEnabled: true`。

客户端 URL 保存在 `build/hy2-ab-client.txt`。客户端需支持 Hysteria 2，并允许 UDP 转发。
