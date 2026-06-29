# xgw 产品化收敛快速说明

## 极简配置

产品侧优先维护一份 `simple.product.json`，只描述入口、认证、证书和线路。

```powershell
python xgw-edge\tools\generate_simple_config.py xgw-edge\examples\simple.product.sample.json xgw-edge\tmp\simple-product-check
```

生成结果：

- `unified.json`：统一配置中间产物。
- `xgw-edge-server.json`：入口前端配置。
- `xgw-edge-client.json`：桌面/模拟客户端配置。
- `xgw-backend.conf`：C 后端 ingress 配置。

## 多线路

`lines[]` 是产品化线路单位，每条线路包含 ingress、relay、egress hop。调度时按线路整体评分，而不是只按单节点评分。

地址生成规则：

- 手机到广州 ingress 使用公网地址。
- 广州 ingress 到香港 relay 优先使用内网地址。
- 香港 relay 到海外 egress 使用公网地址。

## 模拟压测

`xgw-loadgen` 用于在没有真实直播软件和指纹浏览器时先做协议压力验证。

```powershell
go run ./cmd/xgw-loadgen -config tmp/client.json -profile live -target-udp udp.example:443 -duration 30s -concurrency 8 -summary tmp/load-live.json
go run ./cmd/xgw-loadgen -config tmp/client.json -profile browser -target-tcp ip.sb:443 -duration 30s -concurrency 8 -summary tmp/load-browser.json
go run ./cmd/xgw-loadgen -config tmp/client.json -profile mixed -target-tcp ip.sb:443 -target-udp udp.example:443 -duration 30s -concurrency 8 -summary tmp/load-mixed.json
```

输出包含成功率、P50/P95/P99、选中 route、选中 line、错误分布。
