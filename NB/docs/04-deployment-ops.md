# NB V1.5 部署与运维

## 1. 部署前提

生产部署必须准备：

- 三台节点的 SSH 密码环境变量；
- 已核验的 `known_hosts` 文件；
- 私有 CA、三个角色证书和 SOCKS 用户凭据；
- entry 与 exit 的 fail-closed 白名单。
- 广州构建机已安装 `gcc/g++`、`cmake`、`make` 和 OpenSSL 开发包。

当前 `tools/lab-hosts.json` 指定 `build_host=entry`、`compile_dir=/opt/compile`。构建过程只在广州 `/opt/compile` 展开源码和生成中间文件，三端运行目录仍为 `/etc/NB`。`build` 完成后，脚本把二进制下载到本地 `build/nb_node`，并生成 `build/release-manifest.json`。清单绑定二进制 SHA-256、构建输入摘要、拓扑和线路 profile；任一文件在构建后变化，`deploy-socks` 都会 fail-closed，要求重新构建。

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

需要重建 picoquic 静态库时：

```powershell
$env:NB_REBUILD_PICOQUIC = "1"
python tools/deploy.py build
Remove-Item Env:NB_REBUILD_PICOQUIC
```

`/opt/compile` 是可重建目录，不应放置 CA 私钥、SOCKS 明文密码或线上运行日志。`/etc/NB` 是节点运行目录，不再承担源码构建职责。

`tools/security_setup.py` 不会覆盖已有安全目录。`ca.key` 只保留在部署机，节点仅接收 CA 公钥、自己的证书和私钥。

## 2. 部署行为

`deploy-socks` 先在全部节点预上传，再按 `exit -> middle -> entry` 顺序激活：

1. 在三节点取得互斥部署锁，再将二进制和清单写入 `/etc/NB/releases/<deployment_id>/`，逐节点校验 SHA-256；`deployment_id` 由代码 release 和配置摘要组成；
2. 保存当前二进制 symlink 和 systemd unit，所有节点预上传成功后才开始激活；
3. supervisor、TikTok 规则、出口路由和新 unit 与 release 一起版本化；
4. 通过原子 symlink 切换 `/etc/NB/nb_node`，按 hosts JSON 的 `workers` 数量重启；
5. 检查 systemd、实际二进制哈希和每个 worker 的 control socket health；
6. 三节点通过后执行带用户认证的 SOCKS 端到端冒烟；
7. 任一步失败，按相反顺序恢复旧 symlink 和旧 unit，并重启已激活角色。

查看三端当前不可变部署，或精确恢复 canary 之前的完整二进制和参数：

```powershell
python tools/deploy.py current
python tools/deploy.py rollback-socks --deployment-id <release配置摘要>
```

`rollback-socks` 不重新编译；它校验目标目录中的二进制和角色 unit，按 exit、middle、entry 激活并执行 health 与 SOCKS 冒烟。回滚过程中任一节点失败，会把已经切换的节点恢复到回滚开始前的 deployment，避免产生半回滚混跑。

部署前必须设置 SOCKS 冒烟凭据，不能在重启完成后才发现无法验收。部署不再回退 legacy 后台命令。任何发布清单、证书、私钥、用户文件或白名单缺失都会导致启动失败。`deploy-tri` 只用于实验室，不得用于生产发布。

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
python tools/nb_diag.py bundle 500
python tools/nb_observe.py
```

`bundle` 不重启服务，按同一个本地 UTC 采集点拉取三端 release symlink/哈希、systemd 状态、进程、端口、资源、最近 30 分钟 journal、内核异常、NB 日志和所有 worker 的 health/metrics，并生成 `build/incidents/nb-incident-<UTC>.zip`。某一节点不可达时仍保留其余节点证据，并在 `summary.json` 标记采集缺口。

`nb_observe.py` 对累计计数计算窗口差量。节点不可达、health 失败、deployment/profile 混跑立即触发；队龄、有效丢包、重排序、UDP 错误、异常关闭和 event-loop 延迟采用连续窗口判定。自动故障包有 10 分钟冷却，避免同一故障持续写满磁盘。

每个 worker 提供本地控制 socket：

```bash
printf 'health\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl
printf 'metrics\n' | socat - UNIX-CONNECT:/run/nb-middle-0.ctl
```

控制 socket 权限为 `0600`，当前只读，不提供远程修改配置。health 和 metrics 均返回当前 `release_id`、`line_profile` 与 `line_profile_schema`，用于核对三端是否运行同一发布和线路参数版本。

## 6. 测试顺序

1. 运行代码级单元测试；
2. 运行 `scripts/runtri.sh` 安全 loopback 回归；
3. 三跳执行 payload SHA-256 与并发连接测试；
4. 运行自动 netem 矩阵；
5. 保持无 FEC 基线进行手机直播；
6. 仅在基线稳定后启用 FEC active 做对照直播。

三端日志必须按 UTC 时间对齐分析。出现超时应先检查三个角色的所有 worker、控制 socket health、QUIC 握手证书错误和对应下一跳连接池，不得只看 middle/exit 两端。
