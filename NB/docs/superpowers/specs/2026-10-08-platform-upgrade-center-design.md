# NB 平台统一升级中心设计

## 目标

为 NB 控制面提供唯一的版本升级入口，以一个可恢复事务依次完成脚本、控制面和共享 Node 运行时升级。升级允许主动终止现有 Node 会话，由客户端自动重连；单个进程从停止到健康的目标上限为 2 秒。

## 约束

- 升级入口不属于单条线路，线路详情不再提供共享运行时升级操作。
- 共享 Node 升级范围按实际物理设备组合计算，不依赖可修改的资源组名称。
- 保持现有 NB wire format，允许升级期间新旧 Node 短暂混跑。
- 不等待活动会话归零。升级前记录会话数量，滚动重启时允许终止这些会话。
- 现有线路实例配置、端口、证书、白名单和传输 Profile 不得被覆盖或重新生成。
- Node 构建仅在控制面执行。产物只从控制面上传 Entry，再由 Entry 传 Middle、Middle 传 Exit。
- Node 激活顺序固定为 Exit、Middle、Entry；每个角色按 Worker 0、健康检查、Worker 1 顺序滚动。
- 失败时必须自动回滚，不能留下混合的脚本、控制面或 Node 版本而不报告。
- 只有管理员可以构建候选、发起升级或回滚。

## 用户入口

侧边栏新增一级入口“版本升级”，位置在“任务”和“告警”之间。它是升级的唯一写入口。

现有任务页“更新 Node”按钮改为跳转到版本升级页。线路列表和线路详情中的“升级”按钮移除；线路页面只展示当前共享运行时版本及所属升级单元。

版本升级页包含三个无嵌套页面区段：

1. 当前版本：展示脚本快照、Web、Worker，以及各共享节点升级单元的 Entry/Middle/Exit 版本、SHA、Worker 健康和最近升级时间。
2. 候选版本：上传 Git 仓库源码，展示脚本快照、控制面二进制、Node 二进制、构建门禁和回滚信息。构建候选不自动上线。
3. 升级任务：选择候选和升级单元，预览受影响设备与线路，确认“活动连接会被终止并自动重连”后创建任务。

升级任务详情沿用任务时间线，显示预检、脚本切换、控制面切换、Node 分发、各 Worker 激活、冒烟、提交或回滚阶段。

## 升级单元

升级单元由实际设备 ID 三元组和角色组成。任意线路只要共享 Entry、Middle 或 Exit 共享 Shard，就必须被纳入同一个影响集合。显示用资源组名称可以变化，但不参与安全边界计算。

创建升级任务时，服务端重新读取所有活动线路拓扑，计算传递闭包，返回：

- 受影响设备与角色；
- 受影响生产/测试线路；
- 每个 Node Worker 当前版本、SHA、会话数和 control socket 状态；
- 当前控制面和候选版本；
- 可用的上一稳定版本。

客户端提交的影响范围只作为确认摘要，执行时以服务端重新计算结果为准。

## 发布候选

源码上传继续支持 ZIP、TAR、TAR.GZ 和断点续传。上传内容必须是 Git 仓库，但不要求工作区 clean 或填写 commit ID。候选身份由源码内容摘要和构建产物 SHA 确定，Git commit/tree 仅作为审计信息。

控制面构建生成一个统一发布清单：

- `scripts_snapshot`：允许热替换的 Python、配置模板和静态资源摘要；
- `nb_web`：Web 二进制 SHA 与版本；
- `nb_web_worker`：Worker 二进制 SHA 与版本；
- `nb_node`：Node 产品版本、语义版本、release ID 与 SHA；
- `source_digest`、构建时间、构建主机和完整门禁结果；
- 每个组件的上一稳定版本和回滚路径。

控制面必须使用受管 Go 工具链构建 Web/Worker。默认路径为 `/opt/nb-controlplane/toolchains/go/bin/go`；缺失时构建明确失败，不回退到开发机编译。Node 继续使用控制面本地 CMake 构建和完整 CTest/三跳回归。

## 独立升级执行器

新增 `nb-upgrader` systemd 服务和独立可执行文件。它只领取 `platform.upgrade` 和 `platform.rollback` 任务，不领取线路任务。Web、Worker 或脚本被替换和重启时，Upgrader 保持运行并持续写入任务事件。

Upgrader 使用全局升级锁，保证同一时间只有一个平台升级或回滚任务。锁包含操作 ID、候选 release ID、开始时间和心跳；只有超时且执行器确认原进程不存在时才能恢复。

普通 Worker 不执行平台升级。升级进行期间：

- Web 拒绝新的开线、验证、调优、删除、清理和升级写操作，返回当前升级任务；
- 已执行中的普通任务在控制面切换前有最多 30 秒完成时间，超时则失败并保留现场；
- 只读查询、流量图和升级进度保持可用，Web 重启期间前端自动重连。

## 三层事务

### 阶段 1：脚本

Upgrader 将候选脚本解压到版本目录，校验统一发布清单和全部文件 SHA，使用原子软链接切换 `repo/current`。运行中的 Upgrader 使用启动时已打开的版本目录，不受软链接切换影响。此阶段不重启服务。

### 阶段 2：控制面

Web 和 Worker 二进制先写入版本目录并校验 SHA、ELF 架构和 `--version`。切换使用 `.next` 软链接加原子 rename。

顺序为：

1. 停止 Worker 领取新任务；
2. 切换并重启 Worker，2 秒内通过 systemd active 和心跳门禁；
3. 切换并重启 Web，2 秒内通过 systemd active、`/healthz` 和数据库门禁；
4. Upgrader重新连接 Web API并写入阶段完成事件。

任一门禁失败时立即恢复旧软链接并重启对应进程。控制面回滚成功后才允许继续处理普通任务。

### 阶段 3：共享 Node

候选二进制只上传到 Entry；Entry 校验后传到 Middle，Middle 校验后传到 Exit。三端都完成 staging 后才允许激活。

激活顺序为：

1. Exit Worker 0；
2. Exit Worker 1；
3. Middle Worker 0；
4. Middle Worker 1；
5. Entry Worker 0；
6. Entry Worker 1。

每个 Worker 激活前记录活动会话数，原子切换共享二进制软链接并执行 restart。2 秒内必须满足：

- systemd 为 active；
- 目标 control socket 可连接；
- `health` 返回正确角色、Worker 和资源状态；
- 运行二进制版本和 SHA 与候选一致；
- 所有既有实例配置重新加载，数量与升级前一致。

不再以活动会话非零拒绝升级。被终止会话数写入任务证据和审计日志。

单 Worker 失败时先恢复该 Worker。事务失败后，对已升级 Worker 按 Entry、Middle、Exit 和各角色逆 Worker 顺序恢复旧二进制，验证全部 control socket 后再回滚控制面和脚本。

## 冒烟和提交

全部组件升级后执行：

- Web `/healthz`、Worker 心跳和 Upgrader 心跳；
- 所有 Node control socket 与实例配置清单；
- Entry SOCKS 认证；
- TCP 小包、256 KiB 半关闭完整性；
- UDP 生命周期和 2501 字节回显；
- 上下行主动探针；
- 探针 cleanup 后三角色、全部 Worker `active=0`；
- 至少一条生产线路和一条测试线路的端到端冒烟。

主动探针仅用于本次升级验收，完成或失败后都必须清理。全部门禁通过后，候选标记为稳定版本，释放升级锁并恢复普通写操作。

## 数据和接口

沿用 operations/events 存储任务及时间线，新增任务类型：

- `platform.release.build`：构建统一候选；
- `platform.upgrade`：执行三层升级；
- `platform.rollback`：回滚到指定稳定版本。

平台任务使用保留目标 `__platform__`，不伪装成线路。数据库增加平台发布记录，保存统一发布清单、状态、源码摘要、组件 SHA、构建证据和稳定/回滚标记。

新增管理员接口：

- `GET /api/v1/platform-releases/status`
- `POST /api/v1/platform-releases/uploads`
- `PUT /api/v1/platform-releases/uploads/{id}`
- `POST /api/v1/platform-releases/uploads/{id}/complete`
- `GET /api/v1/platform-upgrades/preview?release_id=...&unit=...`
- `POST /api/v1/platform-upgrades`
- `POST /api/v1/platform-upgrades/{id}/rollback`

所有创建接口要求幂等键。服务端拒绝未知字段、过期候选、缺失回滚版本、已有升级锁或影响范围已变化的请求。

## 可观测性和审计

每个阶段记录开始、结束、耗时、目标版本、旧版本、设备、角色、Worker、升级前会话数、健康门禁和回滚结果。任务页面显示单 Worker 实际中断时间，并对超过 2 秒的步骤标记失败。

升级日志不得包含密码、私钥、Token 或完整客户端 URL。日志和发布清单至少保留 90 天，稳定二进制至少保留当前版本和两个历史版本。

## 测试

- Go 单元测试覆盖权限、影响范围闭包、幂等、锁、任务状态和 API 严格 JSON。
- Python 单元测试覆盖脚本原子切换、二进制 staging、两跳转发、2 秒门禁、Worker 顺序和反向回滚。
- Node 测试覆盖强制重启时实例配置恢复、control socket 恢复和探针清理。
- 集成测试使用本地三角色、每角色双 Worker，注入 Worker 启动失败、SHA 错误、Web 健康失败和升级过程中执行器重启。
- 浏览器测试覆盖版本升级导航、候选上传进度、影响线路确认、任务时间线和失败回滚展示。

## 不在本次范围

- Node 无损会话迁移或热补丁；
- 多控制面高可用升级；
- 自动定时升级；
- 在开线任务中隐式升级共享 Node；
- 自动安装来源不明的 Go/C 工具链。
