# Exit 恢复链路与可观测性实施计划

> **供智能执行器使用：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 逐任务实施；所有步骤使用复选框跟踪。

**目标：** 让 Exit 的 DNS 与 TCP 建连阶段具备可追踪的耗时、地址和结果，并让不可达的 TikTok 私网恢复域名快速失败而不是长时间挂起。

**架构：** 扩展异步 DNS 结果携带请求上下文和耗时，在 Exit 主线程做 TikTok-row 私网地址判定；TCP 非阻塞连接使用独立截止时间 helper，并把所有结果写入现有 metrics。DoH 不进入运行时。

**技术栈：** C11、res_nquery、非阻塞 socket/epoll、现有 NB metrics 与控制套接字、CMake/CTest。

**规格：** `docs/superpowers/specs/2026-09-04-media-queue-exit-recovery-design.md`

## 全局约束

- 私网答案快速失败只作用于 `*.tiktok-row.net` 域名，不影响 IP 字面量和其他白名单目标。
- TCP connect 截止时间固定为 5 秒。
- 不伪造公网 DNS 答案，不引入 DoH 依赖。
- 日志说明使用中文；机器可解析字段和 close reason 保持英文。
- 部署失败恢复当前 deployment `5d3187a2956e5b0f-86cf013c8249` 和 transport generation 2。

---

### 任务 1：DNS 请求上下文和私网地址判定

**文件：**
- 修改：`src/nb_dns.h`
- 修改：`src/nb_dns.c`
- 测试：`tools/test_dns.c`

**接口：**
- `nb_dns_result_t` 新增 `host[256]`、`submitted_at_us`、`completed_at_us`。
- 新增 `int nb_dns_tiktok_private_answer(const char *host, const struct sockaddr *address)`。

- [ ] **步骤 1：先写失败测试**

断言 `foo.sg-fn.tiktok-row.net + 10.105.212.98` 返回 1；`rtc-access.tiktokv.com + 10.105.212.98`、TikTok-row 公网地址和 IP 字面量返回 0；异步 literal 查询返回原 host 且完成时间不早于提交时间。

- [ ] **步骤 2：运行红灯测试**

运行：`gcc -std=c11 -Isrc tools/test_dns.c src/nb_dns.c -lresolv -lpthread -o build/test_dns_context`

预期：新字段/API 缺失导致编译失败。

- [ ] **步骤 3：实现上下文传播与后缀/地址校验**

只识别 IPv4 RFC1918 三段：`10/8`、`172.16/12`、`192.168/16`；域名比较不区分大小写并要求标签边界。

- [ ] **步骤 4：运行绿灯测试**

执行同一测试，预期打印 `RESULT PASS`。

### 任务 2：TCP 建连截止时间

**文件：**
- 新建：`src/nb_connect.h`
- 新建：`src/nb_connect.c`
- 修改：`CMakeLists.txt`
- 修改：`tools/deploy.py` 构建输入声明由源码扫描自动覆盖。
- 新建：`tools/test_connect.c`

**接口：**
- `uint64_t nb_connect_deadline_us(uint64_t started_at_us)` 返回开始时间加 5000000，溢出时返回 `UINT64_MAX`。
- `int nb_connect_expired(uint64_t started_at_us, uint64_t now_us)` 仅在已开始且达到 5 秒时返回 1。

- [ ] **步骤 1：先写失败测试**

覆盖未开始、4999999 微秒、5000000 微秒和加法溢出。

- [ ] **步骤 2：运行红灯测试**

运行：`gcc -std=c11 -Isrc tools/test_connect.c src/nb_connect.c -o build/test_connect`

预期：源码/API 不存在导致失败。

- [ ] **步骤 3：实现纯函数并加入 CMake 测试目标**

测试名固定为 `nb_connect_test`，生产库和 `nb_node` 都链接 `src/nb_connect.c`。

- [ ] **步骤 4：运行绿灯测试**

运行 CMake 的单项测试，预期退出 0。

### 任务 3：Exit 状态机接入 DNS 与 connect 恢复

**文件：**
- 修改：`src/nb_node_main.inc`
- 修改：`src/nb_node_core.inc`
- 修改：`src/nb_session.h`
- 修改：`src/nb_instance.h`
- 测试：`tools/test_dns.c`
- 测试：`tools/test_connect.c`

**接口：**
- 消费任务 1 的 DNS 上下文与私网判定。
- 消费任务 2 的截止时间 helper。
- 产出 close reason：`target-dns-fail`、`target-private-unreachable`、`target-connect-timeout`、现有 connect failure reason。

- [ ] **步骤 1：处理 DNS 结果并输出中文说明日志**

日志字段包含 `host/address/dns_ms/result`。TikTok-row 私网答案先计数、reset 上游 stream，再 teardown，不创建 TCP socket。

- [ ] **步骤 2：把 TCP connect deadline 加入 epoll 等待时间计算**

每轮事件循环计算所有 `tcp_connecting` 流的最近截止时间；到期时关闭 fd、reset 上游并以 `target-connect-timeout` 回收一次。

- [ ] **步骤 3：保留正常完成路径**

正常 `SO_ERROR==0` 仍标记 `target_connect_state=2`；记录最大 connect latency，不改变 `live-netacc/rtc-access` 行为。

- [ ] **步骤 4：运行 DNS、connect 和三跳生命周期测试**

运行：`ctest --test-dir test-build -R "nb_(dns|connect|udp_lifecycle)_test" --output-on-failure`。

### 任务 4：metrics 与控制面证据

**文件：**
- 修改：`src/nb_metrics.h`
- 修改：`src/nb_metrics.c`
- 修改：`src/nb_node_core.inc`
- 修改：`tools/test_metrics.c`

**接口：**
- 新增累计字段：`dns_requests`、`dns_failures`、`dns_private_rejected`、`dns_latency_max_us`、`target_connect_timeouts`、`target_connect_latency_max_us`。
- JSON 新增对象 `exit_connectivity`，保持已有字段兼容。

- [ ] **步骤 1：先写失败测试**

调用新的 note API 后断言 `exit_connectivity` JSON 包含所有字面量计数和最大耗时。

- [ ] **步骤 2：运行红灯测试**

运行：`gcc -std=c11 -Isrc tools/test_metrics.c src/nb_metrics.c -o build/test_metrics_exit.exe`，预期编译失败。

- [ ] **步骤 3：实现饱和累加和最大值统计**

计数使用现有 `UINT64_MAX` 饱和模式，最大耗时只增不减；非 Exit 角色自然保持零值。

- [ ] **步骤 4：运行绿灯测试和 JSON 解析检查**

测试程序退出 0，输出可以被 Python `json.loads` 解析。

### 任务 5：合并构建、事务部署和恢复验证

**文件：**
- 使用：`tools/deploy.py`
- 使用：`tools/line_probe.py`
- 使用：`build/sync_candidate_controlplane_repo.py`
- 新建：`build/verify_candidate_media_exit_fix.py`

**接口：**
- 输入：媒体队列任务的源码、Exit 恢复任务的源码、当前线路安全材料。
- 输出：新 release/deployment、六 worker readback、媒体突发和私网 DNS 快速失败证据。

- [ ] **步骤 1：同步源码并在广州2执行完整构建门禁**

必须看到 CMake 成功、全部 CTest 通过、三跳 integrity/UDP lifecycle smoke 通过。

- [ ] **步骤 2：等待共享角色活动会话清零**

不杀进程、不跳过 session drain；超时则保持当前版本并报告。

- [ ] **步骤 3：按 Exit、Middle、Entry 事务发布**

任一角色失败，使用现有部署回滚恢复三个角色。

- [ ] **步骤 4：运行自动验收**

断言六个 worker 的 release、deployment、generation 2 一致；租户上下行仍为 5000Kbps且突发为 `2500000/625000`；`signal_direct=false`。

- [ ] **步骤 5：运行手机与观看端验收**

记录点击、进入、红黄绿和两次观看端卡顿时间；验证期间 `queue_pressure_dropped` 增量必须为 0，私网恢复请求必须快速失败且计数可见。

- [ ] **步骤 6：提交 Exit 修复和部署证据**

仅暂存本计划涉及文件，提交说明使用中文：`修复：增加Exit恢复链路超时与可观测性`。
