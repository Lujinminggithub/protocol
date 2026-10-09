# Newbility（NB）

当前产品版本：`1.0.1.5`。

NB 是基于 picoquic 的可级联三跳传输系统。生产基线 `v1.5` 保留可靠 TCP-over-QUIC 语义，拓扑为 `entry -> middle -> exit -> target`；P2 服务端 A/B/C 已按受限范围关闭：IPv4-only、FEC observe-only、公共 UDP 单报文不超过 1001 字节。手机 App 暂不开发，未来客户端基于 Xray-core，当前只维护协议收发 demo。

`single_hk` 是兼容的单节点拓扑：同一台香港设备运行 Entry 和 Exit，Entry 通过 `127.0.0.1` 直达本机 Exit，不启动 Middle。客户端接口仍是带用户名/密码的标准 SOCKS5，连接地址为香港设备公网地址和线路 `socks_port`。由于标准 SOCKS5 客户端不承载 NB QUIC，单节点模式固定关闭 NB FEC。

业务策略分为 `general` 和 `tiktok_live`。`general` 是默认通用代理策略，不要求直播吞吐；未配置静态白名单或远程 SRS 时，它允许通过认证的客户端访问通用 IPv4、域名和端口，生产使用应按业务范围收紧规则。`tiktok_live` 使用 TikTok/Teko、媒体端口和严格验证规则，不改变客户端连接格式。

## 当前基线

- 普通 QUIC 主链为默认数据面，自动 FEC 处于观察模式。
- 只有受控测试显式设置 `NB_FEC_V15_ACTIVE=on` 时，纠错型 FEC 才接管候选媒体流。
- 发送路径统一使用 `picoquic_callback_prepare_to_send`，不存在 `picoquic_add_to_stream` 遗留。
- 每个进程使用 `epoll`、环形发送队列、TCP 高低水位背压和 O(1) FEC session 索引。
- worker 数量由 `tools/lab-hosts.json` 分角色配置；当前基线为 entry 2、middle 2、exit 2。
- entry 可按权重选择多个出口；middle 按下一跳地址维护最多 8 个独立 QUIC 连接池。
- P2 受限发布使用有限期 HMAC 豁免绑定证据哈希；三角色故障矩阵不可豁免，active FEC 不属于生产准入面。

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

当前拓扑强制使用广州 Entry 作为生产构建机。源码上传到广州的 `/opt/compile`，完成全部 CTest、CMake 和 picoquic 静态库构建后，Entry 保留权威制品；本地 `build/` 仅保存经过校验的缓存和 `release-manifest.json`。部署时制品按 `Entry -> Relay` 分发，Exit 优先从 Entry 获取，失败时自动改走 `Relay -> Exit`。部署前强校验二进制、源码摘要、拓扑和线路 profile；代码 release 与配置摘要组成不可变 deployment ID，三端使用 `/etc/NB/releases/<deployment_id>/` 保存版本，并通过原子 symlink 激活。构建目录与运行目录相互独立，清理 `/opt/compile` 不影响正在运行的服务。

构建机需要预装 `gcc/g++`、`cmake`、`make`、OpenSSL 开发包、pthread 开发环境以及 `ssh/ssh-keygen`；Relay 需要 `ssh/ssh-keygen` 以承担 Exit 回退分发。构建机、middle、exit 必须使用兼容的 Linux x86_64 ABI。

拓扑文件中的关键配置如下：

```json
"build_host": "entry",
"paths": {
  "compile_dir": "/opt/compile",
  "work_dir": "/etc/NB"
}
```

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

# 3. 在广州 /opt/compile 构建，由 Entry 向后续节点分发
python tools/deploy.py build
python tools/deploy.py deploy-socks
```

`deploy-socks` 使用单次临时 Ed25519 密钥执行节点间断点续传，并使用控制面已核验的目标主机密钥生成临时 `known_hosts`；传输结束后从源端和目标端清理临时密钥。三节点制品和哈希全部通过后再按 exit、middle、entry 激活。systemd、实际二进制哈希、worker control socket 或端到端 SOCKS 冒烟任一失败时，已激活节点自动恢复上一版二进制和 unit。该流程不会自动部署本地尚未重新构建的源码状态。

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

每个 worker 提供本机 `0600` Unix socket：默认实例为 `/run/nb-<role>-<worker>.ctl`，命名实例为 `/run/nb-<instance>-<role>-<worker>.ctl`。共享 Entry/Relay 上的多条线路必须使用不同的 `NB_DEPLOY_INSTANCE`、SOCKS 端口、Entry UDP relay 端口池和 Relay 监听端口；部署文件位于 `/etc/NB/instances/<instance>`，不会覆盖默认实例。

```bash
printf 'health\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl
printf 'metrics\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl

# 单机三角色回归
bash scripts/runtri.sh

# FEC 自动测试与 netem 矩阵
python tools/v15_fec_test.py --help
python tools/netem_matrix.py --help

# 开线后只读探针：生成候选线路参数，不自动应用
python tools/line_probe.py --ping-samples 30 --target-mbps 10

# 主动 QUIC 探针：真实经过 SOCKS 和三跳 QUIC，先校验 echo，再执行受控 sink 负载
# 需要设置 NB_SOCKS_USERNAME 和 NB_SOCKS_PASSWORD
python tools/line_probe.py --active --duration 90 --target-mbps 10
```

候选配置包含逐段 IPv4 DF MTU 探测、运行中 QUIC payload MTU 证据及推荐的 `mtu_max`。
探针只生成候选文件，不直接修改线上参数。

当前生效线路参数属于本地部署配置，默认放在被 Git 忽略的 `build/line-profiles/active.json`，也可通过 `NB_LINE_PROFILE_FILE` 指定；探针输出默认写入 `build/line-profile-candidate.json`。仓库不保存包含真实节点地址或凭据的线路 profile。

代码级测试覆盖 FEC 协议、144 组 RS 擦除组合、环形队列、认证、控制 socket、出口路由和 worker 监督器。真实三跳、netem 和手机直播仍属于部署后的阶段验收。
