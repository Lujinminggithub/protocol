# Newbility（NB）

NB 是基于 picoquic 的可级联 TCP-over-QUIC 三跳传输系统。当前 V1.5 保留可靠 TCP 语义，拓扑为 `entry -> middle -> exit -> target`，并将手机原生客户端留到 V2。

## 当前基线

- 普通 QUIC 主链为默认数据面，自动 FEC 处于观察模式。
- 只有受控测试显式设置 `NB_FEC_V15_ACTIVE=on` 时，纠错型 FEC 才接管候选媒体流。
- 发送路径统一使用 `picoquic_callback_prepare_to_send`，不存在 `picoquic_add_to_stream` 遗留。
- 每个进程使用 `epoll`、环形发送队列、TCP 高低水位背压和 O(1) FEC session 索引。
- 部署默认每个角色启动 2 个独立 worker，使用 `SO_REUSEPORT` 接收内核分流。
- entry 可按权重选择多个出口；middle 按下一跳地址维护最多 8 个独立 QUIC 连接池。

## FEC V1.5

FEC 已独立到 `src/nb_fec.c`，数据面采用 QUIC datagram 的 source/repair 分片，控制面负责 block meta、NACK、RETX、RETX ACK、block ACK、FIN/FIN ACK 和 RST。

当前实现包含：

- GF(256) Reed-Solomon `RS(k,r)`；
- 多 block 乱序接收窗口；
- 按 BDP 计算的 TX history；
- NACK 重试与稀疏可靠补发；
- 严格协议长度、索引、状态和 FIN 边界校验；
- payload 完整性、乱序、恢复、重试和 netem 矩阵测试。

## 安全基线

生产模式为 fail-closed：

- 三个角色都必须提供 `-c <cert> -k <key> -a <ca>`；
- middle 和 exit 强制校验客户端证书，entry 和 middle 校验下游服务端证书；
- SOCKS5 entry 必须提供 RFC 1929 用户认证文件 `-U`；
- SOCKS 密码只保存 PBKDF2-SHA256 派生值；
- SOCKS entry 与 exit 都必须提供 `-W <whitelist>`；
- 私钥和用户文件必须由运行用户持有，权限为 `0600` 或更严格；
- 部署主机密码来自环境变量，SSH 默认要求 `NB_KNOWN_HOSTS`。

`NB_INSECURE_TEST_MODE=1` 只用于隔离实验，不得用于线上。

## 构建与部署

```powershell
# 1. 设置 SSH 凭据和主机指纹文件
$env:NB_SSH_PASSWORD_ENTRY = "..."
$env:NB_SSH_PASSWORD_MIDDLE = "..."
$env:NB_SSH_PASSWORD_EXIT = "..."
$env:NB_KNOWN_HOSTS = "C:\path\to\known_hosts"

# 2. 一次性生成私有 CA、节点证书和 SOCKS 凭据
$env:NB_SOCKS_USERNAME = "..."
$env:NB_SOCKS_PASSWORD = "..."
python tools/security_setup.py

# 3. 构建并部署
python tools/deploy.py build
python tools/deploy.py deploy-socks
```

首次引导且尚未建立 known_hosts 时，可以显式设置 `NB_SSH_INSECURE=1`，完成指纹核验后应立即取消。

## 多出口

在 hosts JSON 中增加 `exits`：

```json
"exits": [
  {"name": "kz-a", "host": "203.0.113.10", "port": 4443, "weight": 2},
  {"name": "kz-b", "host": "203.0.113.11", "port": 4443, "weight": 1}
]
```

部署脚本会生成 `exit_routes.conf`。entry 按权重选择 `H:host:port`，middle 为每个出口建立独立连接池。新增出口服务器仍需部署对应的 exit 服务进程。

## 控制面与测试

每个 worker 提供本机 `0600` Unix socket：`/run/nb-<role>-<worker>.ctl`。

```bash
printf 'health\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl
printf 'metrics\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl

# 单机三角色回归
bash scripts/runtri.sh

# FEC 自动测试与 netem 矩阵
python tools/v15_fec_test.py --help
python tools/netem_matrix.py --help
```

代码级测试覆盖 FEC 协议、144 组 RS 擦除组合、环形队列、认证、控制 socket、出口路由和 worker 监督器。真实三跳、netem 和手机直播仍属于部署后的阶段验收。
