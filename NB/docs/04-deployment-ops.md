# NB 部署运维文档

> [01-需求](01-requirements.md) | [02-架构](02-architecture.md) | [03-已实现](03-implementation.md) | 04-部署运维(本篇) | [05-路线图](05-roadmap.md)

## 1. 机器清单 (`tools/lab-hosts.json`)

| 角色 | 名称 | 公网IP | SSH端口 | 说明 |
|---|---|---|---|---|
| entry | gz-83 | 106.75.169.83 | 22 | 广州，SOCKS5 入口 :1080，UCloud |
| middle | hk-216 | 152.32.171.216 | 22 | 香港，QUIC :4443，经 entry 跳板 direct-tcpip(jump_target 172.16.31.28) |
| exit | kz-1 | 2.135.147.102 | 5222 | 哈萨克，QUIC :4443，**构建机**，白名单在此 |

- 工作目录 `WORK=/root/nb`。证书 `/root/nb/certs/{cert,key}.pem`(vendored 测试证书)。
- SSH 密码在 lab-hosts.json。middle 经 entry 跳板连接。

## 2. deploy.py 命令 (`python tools/deploy.py <action>`)

| 命令 | 作用 |
|---|---|
| `recon` | 只读侦察各机 arch/gcc/cmake/证书/nb进程 |
| `build` | 在 exit(kz,构建机) CMake 编译 → 产物下载本地 `build/nb_node` |
| `deploy-socks` | 分发本地 `build/nb_node` 到三跳 + 优先 `systemd` 重启、无 unit 自动回退 legacy 启动 + 冒烟(ipinfo.io出口IP=kz) |
| `deploy-tri` | 三跳固定 route 模式 + md5 冒烟(实验脚本, 与 systemd 常驻服务分离使用) |
| `stop` | 停三跳 nb_node |
| `logs` | 拉三跳最近日志 |
| `wl-push` | 下发 `tools/whitelist.local.conf` → exit + 热重载(首次自动创建模板) |
| `wl-show` | 显示 exit 当前 whitelist.conf |
| `fec-status` | 查看 middle 当前 FEC drop-in / Environment / 日志尾部 |
| `fec-on` | 在 middle 写入 systemd drop-in `NB_FEC=on` 并重启 |
| `fec-off` | 删除 middle FEC drop-in 并重启，回滚单发 |

**标准发布流程**：改 `src/nb_node.c` → `python tools/deploy.py build`(看 `BUILD_OK`) → `python tools/deploy.py deploy-socks` → 验证。

**当前脚本策略**：`deploy.py` 优先走 `systemd`（服务名 `nb-entry` / `nb-middle` / `nb-exit`）；若目标机尚未安装对应 unit，会自动回退到 legacy `nohup` 启动，兼容当前混合环境。

## 3. 启动命令（三角色）

```bash
# entry (SOCKS5, 手机连 1080, 三跳前缀 H:kz)
nb_node -r entry -l 1080 -n <hk_ip> -N 4443 -S -M 'H:<kz_ip>:4443' [-W <whitelist>]
# middle
nb_node -r middle -p 4443 -c <cert> -k <key>
# exit (带白名单)
nb_node -r exit -p 4443 -c <cert> -k <key> -W /root/nb/whitelist.conf
# 环境变量 NB_LOG_LEVEL=DEBUG 开调试日志
# 环境变量 NB_FEC=on 仅需配置在 middle，用于直播延迟流双发
```
命令行参数：`-r`角色 `-l`SOCKS端口 `-p`QUIC端口 `-n/-N`第一跳host/port `-S`SOCKS模式 `-M`中间跳前缀 `-R`固定route `-c/-k`证书 `-W`白名单文件。

## 4. 白名单管理

- **本地维护**：`tools/whitelist.local.conf`(不入库)。语法：
  ```
  domain baidu.com      # 后缀匹配, =*.baidu.com(所有子域)
  ip 110.242.68.0/24    # CIDR
  port 443
  ```
  每维度独立，某类为空=不限制该维度。
- **下发**：`python tools/deploy.py wl-push` → exit `/root/nb/whitelist.conf`，~5s 自动热重载(改 mtime 触发)。
- **验证**：exit 日志 `whitelist loaded: N domains, M cidrs, K ports`。拦截记录 `BLOCKED ...(WARN级)`。
- **默认模板**：deploy.py 的 `WL_DEFAULT` 常量(含 TikTok+baidu 域名+端口)，仅当 exit 无文件时推送,不覆盖远程改动。

## 5. 手机配置

Shadowrocket → 新建 SOCKS5 代理 → `106.75.169.83:1080`，无认证。
- 历史文档里的 `28443` 已废弃，以 `1080` 为准。

## 6. 诊断工具 (`tools/nb_diag.py`)

| 命令 | 作用 |
|---|---|
| `probe` | 只读拉三跳日志统计(CONNECT/PSFREE/泄漏) + entry 连接数 |
| `stress [N]` | 并发 N 压测 + 三跳差量归因 |
| `coldtest [N]` | 重启 entry 隔离 + 连压 3 轮，验证无泄漏/无雪崩(关键回归测试) |
| `throughput [MB]` | exit 本地大文件，测单流/5/20并发吞吐 |

**注意**：coldtest 默认测 `www.google.com`(现被白名单拦会失败,属正常)。要测功能改用白名单内域名或临时加白名单。

## 6.1 FEC 运维

- 当前 FEC 实现为“仅 middle 对直播延迟流双发，exit/middle 两端按 `flowid` 去重”，默认关闭。
- 开启：`python tools/deploy.py fec-on`
- 查看：`python tools/deploy.py fec-status`
- 回滚：`python tools/deploy.py fec-off`
- 线上判断：`/root/nb/logs/nb-middle.log` 出现 `FEC dual-send: ENABLED (NB_FEC=on)` 即代表已生效。

## 7. 诊断链路质量（直播红黄定位）

拉 entry/middle 的 `linkq` 日志：
```bash
ssh root@106.75.169.83 "grep linkq /root/nb/logs/nb-entry.log|tail"   # gz→hk 段
ssh -p5222 root@2.135.147.102 "grep linkq /root/nb/logs/nb-middle.log" # hk→kz 段(middle在hk但日志...实际middle日志在hk机)
```
`linkq pool[k] rtt=Xms lost_pkt=N cwin=YKB`。哪段 lost_pkt 涨快/rtt 抖 = 抖动源。

## 8. 云安全组（重要坑）

- gz(UCloud) 安全组需放行手机入口端口 `1080`，以及运维/测试需要的 22、8000/8080/9000 等端口。
- **kz(境外)测 gz 端口 TIMEOUT，但手机(境内)能连** → 安全组按来源地域放行。所以 kz 探针≠手机可达性，**手机实测为准**。
- 改端口需先在云控制台放行；UCloud 外网防火墙是独立资源要绑定到主机才生效(踩过：配了没绑定→不生效)。

## 9. 构建系统

- `CMakeLists.txt`：prebuilt 静态库(`third_party/picoquic/prebuilt/linux-x86_64/*.a`)直接链，缺失则 `build_libs.sh` 从源码现编。
- `deploy.py build` 在 kz 编译，`BUILD_FILES` 传 src+CMakeLists+picoquic头文件+prebuilt到 kz。
- **⚠️ kz 编译环境的 picoquic.h 是旧版**，`picoquic_path_quality_t` 只有 rtt/lost/cwin 等基础字段(无 rtt_min/max/variant)。加新字段前需在 kz 上验证存在。
