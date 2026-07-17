# NB V1.5 部署与运维

## 1. 部署前提

生产部署必须准备：

- 三台节点的 SSH 密码环境变量；
- 已核验的 `known_hosts` 文件；
- 私有 CA、三个角色证书和 SOCKS 用户凭据；
- entry 与 exit 的 fail-closed 白名单。

```powershell
$env:NB_SSH_PASSWORD_ENTRY = "..."
$env:NB_SSH_PASSWORD_MIDDLE = "..."
$env:NB_SSH_PASSWORD_EXIT = "..."
$env:NB_KNOWN_HOSTS = "C:\secure\nb_known_hosts"
$env:NB_SOCKS_USERNAME = "..."
$env:NB_SOCKS_PASSWORD = "..."

python tools/security_setup.py
python tools/deploy.py build
python tools/deploy.py deploy-socks
```

`tools/security_setup.py` 不会覆盖已有安全目录。`ca.key` 只保留在部署机，节点仅接收 CA 公钥、自己的证书和私钥。

## 2. 部署行为

`deploy-socks` 按 `exit -> middle -> entry` 顺序执行：

1. 分发 `nb_node` 和角色专属安全材料；
2. 下发 TikTok 分流规则、白名单和出口路由文件；
3. 原子安装 systemd unit；
4. 按 hosts JSON 的 `workers` 数量启动监督器；
5. 检查服务状态并执行带用户认证的 SOCKS 冒烟。

部署不再回退 legacy 后台命令。任何证书、私钥、用户文件或白名单缺失都会导致启动失败。

## 3. FEC 模式

默认 unit 设置 `NB_FEC_V15=on`，含义是只观察门控，不接管业务数据。普通 QUIC 是稳定基线。

```powershell
python tools/deploy.py fec-status
python tools/deploy.py fec-on   # 受控测试：设置 NB_FEC_V15_ACTIVE=on
python tools/deploy.py fec-off  # 回到观察模式
```

线上直播验收前应先保持观察模式完成普通 QUIC 基线测试，再在 netem 或隔离线路上启用 active 模式。

## 4. 多出口

hosts JSON 可增加：

```json
"exits": [
  {"name": "kz-a", "host": "203.0.113.10", "port": 4443, "weight": 2},
  {"name": "kz-b", "host": "203.0.113.11", "port": 4443, "weight": 1}
]
```

entry 根据权重选择出口，middle 根据下一跳地址建立独立连接池。每台新增出口服务器必须单独部署 exit 服务和角色证书。

## 5. 日志与控制面

```powershell
python tools/deploy.py logs
python tools/nb_diag.py probe
```

每个 worker 提供本地控制 socket：

```bash
printf 'health\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl
printf 'metrics\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl
```

控制 socket 权限为 `0600`，当前只读，不提供远程修改配置。

## 6. 测试顺序

1. 运行代码级单元测试；
2. 运行 `scripts/runtri.sh` 安全 loopback 回归；
3. 三跳执行 payload SHA-256 与并发连接测试；
4. 运行自动 netem 矩阵；
5. 保持无 FEC 基线进行手机直播；
6. 仅在基线稳定后启用 FEC active 做对照直播。

三端日志必须按 UTC 时间对齐分析。出现超时应先检查三个角色的所有 worker、控制 socket health、QUIC 握手证书错误和对应下一跳连接池，不得只看 middle/exit 两端。
