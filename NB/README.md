# Newbility (NB) —— 自研可级联 TCP-over-QUIC 传输

基于 **picoquic (BBRv2)** 的多跳 TCP-over-QUIC 隧道。一份二进制 `nb_node`,靠 `-r` 选角色,
组成 **entry → middle(×N) → exit** 的可变跳数链路。路径由 entry 下发的 source-route 决定,
**中间节点无状态**(下一跳地址来自 stream 首部),天然适配"卖线路 / 动态选路 / 路径探测"产品模型。

## 角色

| 角色 | left(靠 client) | right(靠 target) | 说明 |
|------|-----------------|------------------|------|
| entry  | 本地 TCP accept | 下游 QUIC(第一跳) | 每 TCP 连接开一个 QUIC bidi stream,首部发 route |
| middle | 上游 QUIC(上一跳) | 下游 QUIC(下一跳) | 读首部 `H:host:port` 动态连下一跳,转发剩余 route + 数据 |
| exit   | 上游 QUIC(上一跳) | 目标 TCP | 读首部 `T:host:port`,connect 目标,双向泵 |

## source-route

stream 首部第一行(文本 + `\n`),语义 = 接收节点出发的剩余路径:
```
H:hop2:qport,...,T:targethost:targetport
```
- `H:` = 下一 QUIC 跳地址(middle 消费第一个 H,转发其余);
- `T:` = 最终 TCP 目标(exit 消费);
- 两跳(entry 直连 exit)时 route 仅 `T:target:port`(无 H)。

例(三跳 gz→hk→kz→target): entry `-n hk -R "H:kz:4443,T:target:80"`
→ hk 消费 `H:kz:4443` 连 kz、转发 `T:target:80` → kz 连 target。

## 目录

```
src/nb_node.c             统一三角色节点
src/log/log4c.{c,h}       结构化日志(route+stream_id 贯穿三跳可 grep 追踪)
third_party/picoquic/     vendored picoquic(源码 + 平台预编译静态库) —— 见 docs/design.md
tools/deploy.py           运维入口: recon / build / deploy-socks / wl-* / fec-* / logs
tools/redeploy.py         一键 build + systemd 滚动发布
tools/nb_diag.py          三跳诊断 / 压测 / 吞吐基线
tools/lab-hosts.json      机器清单(entry/middle/exit + 跳板)
tools/vendor_picoquic.py  从构建机抽取 picoquic 固化进 third_party/
scripts/runtri.sh         单机三角色 loopback 回归(不依赖真机)
docs/design.md            架构 / 协议 / BBRv2+FEC 规划 / 验证结果
```

## 用法

```bash
# 1) 编译(在有 picoquic 的机器, 产物下载到 build/)
python tools/deploy.py build

# 2) 分发到三跳 + 优先 systemd、无 unit 自动回退 legacy + SOCKS5 冒烟(默认 1080)
python tools/deploy.py deploy-socks

# 或一键 build + deploy
python tools/redeploy.py

# 3) FEC 双发开关(仅 middle 需要开)
python tools/deploy.py fec-status
python tools/deploy.py fec-on
python tools/deploy.py fec-off

# 4) 查三跳日志/诊断
python tools/deploy.py logs
python tools/nb_diag.py probe

# 停
python tools/deploy.py stop

# 单机 loopback 回归(一台机起 entry+middle+exit)
bash scripts/runtri.sh /path/to/picoquic
```

## 命令行

```
entry : nb_node -r entry  -l <tcp_port> -n <hop1_host> -N <hop1_qport> -R <route>
middle: nb_node -r middle -p <quic_port> -c <cert> -k <key>
exit  : nb_node -r exit   -p <quic_port> -c <cert> -k <key>
```

## 验证状态(2026-07-05)

真实地理三跳 **gz(广州)→hk(香港)→kz(哈萨克斯坦)→target** 打通，当前线上入口为 `106.75.169.83:1080`:
- 5 并发 stream 全部 http=200,300KB md5 零损坏穿三跳;
- first_byte: 冷启动 ~400ms(含握手),复用 ~200ms;
- middle 正确做 `H:` 逐跳消费转发,log4c route+stream_id 三跳可追踪。
- 直播延迟流已支持基于 `flowid` 的 FEC 双发/去重，默认关闭；middle 用 `NB_FEC=on` 开启。

后续: 量化 FEC 在丢包场景下的收益(tc netem)、按链路质量自适应开关双发、以及控制平面(探测+选路+下发)。
