# gateway -> xgw-edge 迁移说明

旧 `gateway/` 目录先保留，不删除。

当前已经并入 `xgw-edge/` 的能力：

- QUIC + TLS1.3 + HTTP/3 服务端入口
- `POST /auth` 风格控制面
- 未认证伪装回源 / 普通 HTTP fallback
- QUIC datagram <-> UDP backend 转发骨架
- keepalive 控制面
- 节点池与评分选路
- reconnect hint / route update 控制面

建议后续迁移顺序：

1. 以 `xgw-edge/cmd/xgw-edge-server` 作为新的默认入口
2. 将 `gateway/` 中剩余 metrics / 日志字段逐步搬到 `xgw-edge`
3. smoke / deploy 脚本优先切换到 `xgw-edge`
4. 待功能完全覆盖后，再冻结 `gateway/`
