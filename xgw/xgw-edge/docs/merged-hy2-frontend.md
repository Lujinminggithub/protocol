# xgw-edge-server 与 hy2 官方前端的合并方式

当前不再把 `hy2-bridge` 当作独立产品入口对外暴露。

## 对外目标

对外只保留一个服务端名字：

- `xgw-edge-server`

## 当前实现方式

由于本地 `hysteria/core` 与现有 `xgw-edge` 主模块的 `quic-go` 依赖树不同，暂时不做代码级硬合并。

因此当前采用的是：

- 统一使用一份 `server.json`
- 统一打包到 `release/linux-server`
- 统一通过 `start-server.sh` 启动
- 统一对外二进制名字为 `xgw-edge-server`

当：

- `compatibility.mode = hy2-official-bridge`

时，发布目录里的 `xgw-edge-server` 实际上由官方 HY2 bridge 前端实现提供能力。

## 模式说明

- `hy2-official-bridge`
  - 当前默认推荐模式
  - 用于 Shadowrocket / 第三方 HY2 客户端接入
- `native`
  - 只保留在源码中用于开发与自有客户端验证
  - 不作为当前手机端测试主路径

## 后续方向

- 继续把 `xgw-edge-server` 的后端控制、节点池、路由策略逐步吸收到官方 HY2 前端路径中
- 等依赖树条件成熟，再尝试真正的代码级合并
