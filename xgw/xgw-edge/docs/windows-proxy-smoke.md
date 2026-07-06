# Windows 本地代理最小使用说明

当前 `xgw-edge client` 已支持：

- 本地 `SOCKS5 CONNECT`
- 本地 `SOCKS5 UDP ASSOCIATE`
- 本地 `HTTP CONNECT`

配置项在 `client.json` 里：

- `local_socks5_listen`
- `local_http_listen`
- `local_mode`

推荐最小配置：

```json
{
  "local_socks5_listen": "127.0.0.1:10808",
  "local_http_listen": "127.0.0.1:18080",
  "local_mode": "udp"
}
```

## 启动

```powershell
.\xgw-edge-client.exe -config .\client.json
```

前提：

- `client.json` 里的 `server_url` 指向一个已经运行的 `xgw-edge-server`
- 例如先在 Linux 远端或本机另一终端启动：

```powershell
.\xgw-edge-server.exe -config .\server.json
```

## HTTP CONNECT 验证

```powershell
curl.exe -v --proxy http://127.0.0.1:18080 http://127.0.0.1:8443/
```

## SOCKS5 CONNECT 验证

```powershell
curl.exe -v --proxy socks5h://127.0.0.1:10808 http://127.0.0.1:8443/
```

## Windows 软件接入

- OBS:
  通过 Proxifier 把 `obs64.exe` 的 TCP 连接导向 `127.0.0.1:10808` 或 `127.0.0.1:18080`
- 直播伴侣:
  优先通过 Proxifier 把进程导向本地 SOCKS5/HTTP CONNECT

## Proxifier 最小配置

1. 打开 `Profile -> Proxy Servers`
2. 新增一个 `SOCKS5` 代理：
   `Address = 127.0.0.1`
   `Port = 10808`
3. 打开 `Profile -> Proxification Rules`
4. 新增规则：
   `Applications = obs64.exe` 或直播伴侣主进程
   `Action = Proxy SOCKS5 127.0.0.1:10808`
5. 保存后重启目标程序

如果目标程序更适合 HTTP CONNECT，也可以配：

1. `Profile -> Proxy Servers`
2. 新增一个 `HTTPS / HTTP` 代理：
   `Address = 127.0.0.1`
   `Port = 18080`
3. 将规则动作指向这个 HTTP 代理

## 当前限制

- 直播软件若强依赖 UDP 直连能力，仍建议继续补 Windows TUN / Wintun
- SOCKS5 UDP ASSOCIATE 当前是最小实现，优先用于验证链路接入，不是最终产品级 NAT 行为模型
