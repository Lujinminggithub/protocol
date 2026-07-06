# xgw-edge 对齐 HY2 官方服务端清单

当前目标不是完全重写成 HY2，而是让 `xgw-edge` 作为 **HY2-compatible frontend**。

## 第一阶段 已完成

- `Hysteria-Auth` 请求头兼容
- `Hysteria-UDP` / `Hysteria-CC-RX` 响应头兼容
- `233` 认证成功状态语义
- QUIC stream 基础 TCPRequest/TCPResponse
- UDPMessage 基础编解码

## 第二阶段 已开始

- 按 `SessionID` 建立 UDP 会话
- 为每个会话维护独立 UDP socket
- 回包时保留原始地址语义
- idle timeout 清理

## 仍待继续对齐

- 更完整的 UDP 分片 / 重组行为
- 更接近 HY2 的 bandwidth negotiation 细节
- 更接近 HY2 的错误语义与关闭码
- 更完整的 TCP/UDP 代理事件日志
- 更自然的 H3 伪装行为

## 当前判断

到这一阶段后，`xgw-edge` 已从“只兼容认证头”推进到“兼容头 + 基础 UDP session 语义”。
是否足够让 Shadowrocket 成功接入，仍需要真机验证。
