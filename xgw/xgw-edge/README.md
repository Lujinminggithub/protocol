# xgw-edge

`xgw-edge` 是面向产品化的 Go 传输面，负责承接：

- QUIC/TLS/HTTP3 级安全模型
- 成熟的控制面握手、带宽协商、keepalive 与恢复
- 普通 HTTP 伪装与未认证流量伪装回源
- 边缘节点池、评分与自动选路
- 向本地 `xgw` 数据面或通用 UDP/TCP 后端转发

当前目录下的新实现用于替代早期 `gateway/` 目录里偏实验性的脚手架。

## 当前实现覆盖

- `cmd/xgw-edge-server`：服务端入口
- `cmd/xgw-edge-client`：客户端入口
- `internal/config`：配置模型与默认值
- `internal/control`：控制面请求/响应模型
- `internal/pool`：节点池加载、评分、选路
- `internal/masq`：伪装 HTTP/未认证回源
- `internal/edge`：QUIC/H3 服务端与客户端骨架

## 后续继续深化

- 接入真实 ACL / outbounds / DNS / 资源池遥测
- 接入 uTLS / 指纹伪装 / ECH / 多证书策略
- 接入更成熟的 HTTP/3 页面级伪装素材
- 接入控制流与数据流的版本演进、重连恢复、会话迁移
