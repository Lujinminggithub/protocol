# 06 自我保护与统一解锁/卸载

> 上级：[README](./README.md) ｜ 相关：[02 内核架构](./02-kernel-architecture.md)
> 决策：ADR D6（认证 A+B）、D7（范围）、D8（全局解锁+短 TTL）。

## 设计原则：默认锁死，凭票放行

自保护 = 内核状态机「LOCKED（默认）↔ UNLOCKED（凭合法票据）」。**手动放开、自己卸载、白名单卸载全部收敛为同一条路线**：取得 unlock 票据 → 进入 UNLOCKED；区别只在解锁后做什么。

```
保护面(拦截)         解锁面(认证)              卸载面(收尾)
文件/目录     ┌ IOCTL_PS_UNLOCK ┐        卸载器=先解锁→再删文件/停服务/卸驱动
进程防杀   ◄禁止─ A+B 双因子认证 ─放行►
防卸载        └ 全局解锁+短TTL  ┘
```

## 保护范围（ADR D7）

| 面 | 手段 | 备注 |
|---|---|---|
| 文件/目录 | 复用 MiniFilter：`PreCreate`(DELETE意图)、`PreSetInformation`(删除/改名)、`PreWrite`(覆盖) 命中受保护路径→拒绝 | 防删主战场 |
| 进程防杀 | `ObRegisterCallbacks` 剥离 `PROCESS_TERMINATE/VM_WRITE/VM_OPERATION/CREATE_THREAD/SUSPEND_RESUME` | ⚠️需驱动 `/INTEGRITYCHECK` 有效签名 |
| 防卸载 | `FLT_REGISTRATION.FilterUnloadCallback` 检查状态：LOCKED→`STATUS_FLT_DO_NOT_DETACH` | 不阻止系统关机强制卸载 |

> **边界提醒**：未选注册表/服务保护。后果——运行期驱动在内存受保护，但 `sc delete` 可删服务键致**下次开机不加载**（本次不受影响）。若要闭合，最小补一个服务键的 `CmRegisterCallbackEx` 保护。

## 状态机与数据结构（设计层）

```
PS_PROTECT_STATE {
  LONG Enabled;            // 总开关(策略), 默认1
  LONG Mode;              // LOCKED(默认) / UNLOCKED
  LARGE_INTEGER UnlockExpire;  // TTL到期绝对时间
  UCHAR Nonce[32];        // 当前挑战
  LARGE_INTEGER NonceIssued;   // 签发时刻(~30s有效)
  BOOLEAN NonceConsumed;  // 一次性
  ULONG ProtectedPids[K]; // 受保护进程快照
  PS_PATH_PREFIX FilePrefixes[N]; // 受保护路径前缀(NORMALIZED)
}
```

状态流转：`加载→LOCKED` → `GET_CHALLENGE发nonce` → `UNLOCK(验签+身份OK)→UNLOCKED设TTL` → `TTL到期/RELOCK→LOCKED` → `卸载完成→驱动出内存`。

- 默认 **LOCKED**（fail-safe），绝不留「下发前敞开」窗口。
- 读多写少，`EX_PUSH_LOCK`；TTL 惰性检查 + `KTIMER` 兜底。
- 统一判断：`ShouldEnforce() = Enabled && Mode==LOCKED && 未过期`。

## 当前实现状态（2026-07-16）

- `src/kernel/core/protect.c` 已实现默认锁定和单一 `IOCTL_PS_PROTECT_CONTROL`。首次连接使用内置固定令牌执行 `INITIALIZE`，维护删除使用同一 IOCTL 的 `UNLOCK`，完成后可 `RELOCK`；解锁仍保留最长 300 秒自动回锁兜底。
- 文件保护已前置到普通 DLP 策略和隔离重定向之前，覆盖破坏性 create、write、delete、rename 以及可在安全 IRQL 获取名称的 paging write。
- `ObRegisterCallbacks` 已接入受保护 PID 快照和映像路径清单。配置时显式 PID 立即生效；之后匹配映像路径的新建/重启进程由进程通知自动加入并在退出时移除。锁定时剥离终止、写内存、创建线程、挂起、复制句柄等高危权限；驱动工程已启用 `/INTEGRITYCHECK`。
- `CmRegisterCallbackEx` 已保护 `SYSTEM` 下实际 ControlSet 的 `Services\PersonalSafer` 树，锁定时拒绝删除键、设置/删除值、改名、修改键信息及安全描述符，解锁维护窗口内放行标准卸载流程。
- 锁定时非强制 `fltmc unload` 返回 `STATUS_FLT_DO_NOT_DETACH`；系统强制卸载路径不阻断。
- 原生模块在 `connect()` 内自动发现并保护安装目录、当前主程序映像、当前 PID 和系统驱动文件，不要求调用方传路径、PID、证书或私钥。
- 安装版隐藏维护入口简化为 `--ps-maintenance-action=status|unlock|relock`，结果写入 `--ps-maintenance-out=<目录>` 下的 `protection-maintenance-summary.json`。
- Release WDK 构建和全 PDB 栈帧门禁已通过；保护模块的 CNG 工作区使用 `NonPagedPoolNx`，未把保护清单或密码学工作区放到内核栈。

映像路径清单由签名维护票据授权，并应与同一可执行文件所在路径的文件保护同时配置，防止路径上的映像被替换。当前已运行进程在首次配置时仍需显式传入 PID；之后的重启由映像路径自动跟踪。内核不自行解析 Authenticode，生产配置端应先验证发布者/哈希再让中心签名服务签发维护票据。

设备对象仍需兼容普通用户态审计读取，因此没有直接改成“仅管理员可打开”；五个维护 IOCTL 中涉及 challenge、unlock、清单修改的入口已增加管理员令牌门禁。最终形态应由 SYSTEM 权限 broker 独占控制 IOCTL，再为客户端提供受限 IPC。

安装版维护删除示例：

```powershell
& 'C:\Program Files\PersonalSafer\PersonalSafer.exe' `
  --ps-maintenance-action=unlock `
  --ps-maintenance-out='E:\temp\PersonalSafer-maintenance'
```

固定令牌编译在驱动和原生模块中，只用于首次通信识别和防止误调用；它可以被逆向提取，不应被描述为密码学认证或用于对抗本机管理员。

## IOCTL 接口（走现有 device.c，分发排在保护判断之前）

```
IOCTL_PS_SET_PROTECT_LIST     // 下发保护清单(路径前缀+受保护PID)
IOCTL_PS_GET_CHALLENGE        // 输出: 32字节 nonce
IOCTL_PS_UNLOCK_PROTECTION    // 输入: {nonce, 签名, 机器绑定} → UNLOCKED(TTL)
IOCTL_PS_RELOCK               // 主动重锁
IOCTL_PS_QUERY_PROTECT_STATE  // 输出: {Mode, Enabled, 剩余TTL}
```

设备对象 DACL 收紧：仅 SYSTEM/Administrators/自家进程可打开 `\Device\PersonalSafer`。

## 认证：A+B 双因子（ADR D6）

```
自家进程                                   驱动
 GET_CHALLENGE ───────────────────────────► 生成nonce, 记签发时刻, NonceConsumed=FALSE
     ◄──────────── nonce ───────────────────
 私钥签名(nonce∥MachineGuid∥ts)
 UNLOCK{payload,sig} ──────────────────────► ①因子B(身份): 调用方镜像∈保护清单 且 签名主体==本厂证书
                                            ②因子A(票据): nonce匹配未过期未消费 + 内置公钥验签
                                            ③置UNLOCKED, TTL=now+5min, NonceConsumed=TRUE
     ◄──────────── STATUS_SUCCESS ──────────
```

- **因子 A（验签）**防逆向者伪造 IOCTL；**因子 B（身份）**防合法票据被任意进程重放。
- nonce 一次性 + ~30s + 绑机器，防重放/防跨机。
- 私钥托管（待定）：联网授权服务器签名（最强）vs 私钥内嵌卸载器+混淆（离线可用但弱）。

## 卸载流程（与手动解锁同一条路线）

```
Uninstaller.exe:
 1. GET_CHALLENGE → nonce
 2. 签名 → UNLOCK_PROTECTION         ← 与"手动放开"完全同一接口同一认证
 3. 驱动 → UNLOCKED (TTL 5min)
 4. 保护全旁路, 执行标准卸载:
      taskkill 自家进程 (进程保护旁路) ✔
      fltmc unload (FilterUnload→SUCCESS) ✔
      删安装目录文件 (MiniFilter放行) ✔
      sc delete + 删注册表残留 ✔(本就未拦截)
 5. 完成, 驱动出内存
```

- **手动放开** = 只做 1–3 步（或卸载器 `--unlock`）；TTL 到期自动重锁或 `IOCTL_PS_RELOCK`。
- **"加白卸载"=持有合法票据**；无票者被全面拦截。同一 unlock 接口 = 同一条路线，无卸载后门。

## 安全边界

- 管理员总能通过测试签名/关闭 DSE 等最终绕过——目标是**抬高门槛**，非对抗内核级攻击者。
- 进程/注册表保护需 `/INTEGRITYCHECK` + 有效签名（测试证书在测试签名模式可用；正式版 EV/WHQL）。
- 解锁通道在任何 LOCKED 状态必须可达，认证/分发路径不能被自保护误伤（防自锁死）。
- 解锁/重锁/拦截均出审计事件。

## 待办

- [x] Core `protect` 状态机与单一控制 IOCTL。
- [x] 文件路径、进程、服务注册表树和 FilterUnload 保护。
- [x] 首次固定令牌认证、自动目标发现、重复初始化防覆盖、300 秒 TTL 回锁。
- [x] 安装版 `status/unlock/relock` 维护入口。
- [ ] VM 端到端验证安装目录/驱动删除拒绝、解锁删除放行和 TTL 自动回锁。
- [ ] SYSTEM broker 落地后再将控制设备 DACL 收紧为仅 SYSTEM。

## 涉及文件（规划）

- Core：`src/kernel/core/protect.*`（新增）、`src/kernel/device.c`（IOCTL）
- 文件：`src/kernel/filter/fltmgr.c`（前置保护判断）
- 驱动：`src/kernel/driver.c`（FilterUnload、ObRegisterCallbacks）
- 用户态：`src/native/src/dlp/`（解锁客户端）、卸载器
