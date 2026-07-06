# 真实域名证书后 Shadowrocket 再测最小步骤

如果要提高 Shadowrocket 真机接入成功率，建议按下面步骤重试：

## 1. 准备真实域名

- 例如：`edge.yourdomain.com`
- DNS A 记录指向：`106.75.141.139`

## 2. 申请真实证书

- 使用 Let’s Encrypt / ZeroSSL 等
- 证书域名必须覆盖 `edge.yourdomain.com`

## 3. 修改 xgw-edge server 配置

- `listen = :8443`
- `tls.cert_file = /path/to/fullchain.pem`
- `tls.key_file = /path/to/privkey.pem`
- `masquerade.server_names = ["edge.yourdomain.com"]`

## 4. Shadowrocket 填写

- `类型`: `Hysteria 2`
- `地址`: `edge.yourdomain.com`
- `端口`: `8443`
- `密码`: `edge-secret`
- `SNI`: `edge.yourdomain.com`
- `ALPN`: `h3`
- `UDP 转发`: 开启
- `允许不安全的`: 正式环境建议关闭
- `混淆`: 先留空

## 5. 重试时同步抓服务端现场

建议同时抓：

- `tcpdump udp port 8443`
- `/etc/xgw-edge/logs/server.out.log`
- `/etc/xgw-edge/logs/server.err.log`

这样能明确判断：

- 包有没有到
- 是否进入 auth
- 是否进入 UDP session
