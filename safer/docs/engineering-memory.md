# Engineering Memory

Updated: 2026-07-16

## Fixed in recent passes

- `IOCTL_PS_SET_POLICY` is wired to the kernel policy engine.
- `IOCTL_PS_GET_POLICY` returns the active kernel policy to user mode.
- Driver status reports real `fileFilterActive`, `networkFilterActive`, `lastErrorCode`, and `startTime`.
- DLP 客户端乱码已从源头清理：策略页面移除了重复/损坏模板，恢复中文标签，并补齐策略读取、编辑和保存链。网络过滤状态查询不再让同步 IOCTL 与事件等待错误复用 overlapped 句柄；事件等待使用独立设备句柄，状态读取兼容较短的旧版状态结构。
- Electron 安装资源现在从规范目录 `build/kernel/x64/Release` 获取驱动，并仅在该目录不存在时回退旧输出目录，避免安装包继续携带过期的 `.sys` 导致状态和能力不一致。
- 根目录 `build-all.cmd` 和测试签名入口已同步到规范 WDK 输出目录，不再硬编码失效的 Visual Studio 18 路径或签名旧的 `src/kernel/x64/Release` 驱动。
- WFP status is no longer inferred from "driver loaded"; Electron reads the real driver status.
- WFP ALE auth filters cover both IPv4 and IPv6 connect/accept layers.
- WFP stream inspection callouts are registered on `FWPM_LAYER_STREAM_V4` and `FWPM_LAYER_STREAM_V6`.
- Payload inspection is live for outbound and inbound TCP stream data where applicable:
  - outbound HTTP requests feed `EventHttpRequest`
  - inbound HTTP responses feed `EventHttpResponse`
  - outbound TLS ClientHello / SNI feeds `EventSniCapture`
  - outbound FTP control commands feed `EventFtpCommand`
  - inbound FTP control responses feed `EventFtpCommand`
- FTP control-channel parsing tracks `PORT/EPRT/PASV/EPSV` and associates subsequent FTP data-channel connections in ALE auth.
- FTP transfer lifecycle semantics now distinguish transfer command, transfer start, data-channel hit, and transfer completion/failure context using the control/data-channel state cache.
- FTP data-channel stream inspection now performs a basic content preview pass for matched FTP data flows and emits preview-oriented audit entries.
- FTP `LIST` / `NLST` 数据流现已按数据连接维护跨 buffer 行重组状态，并输出结构化目录项语义。Unix `ls -l`、DOS/Windows FTP 列表、NLST 裸名称、符号链接、UTF-8 BMP 文件名、项类型、大小、时间、owner/group、权限和链接目标会被规范化为 `FTP DIR_ENTRY` 审计事件；传输完成/失败事件会附带目录项、文件、目录、链接数量、列表文件总大小、被抑制事件数量和截断状态。
- FTP `MLSD` 已纳入传输命令和数据通道关联，标准 `type/size/modify/perm/UNIX.owner/UNIX.group/UNIX.mode` facts 会直接映射到统一目录项字段，避免依赖本地化 LIST 文本格式。
- FTP LIST 目录兼容继续扩展到 EPLF 和 OpenVMS/VMS：EPLF facts 会提取文件/目录类型、字节大小和修改时间；VMS 版本化名称会规范化为文件名/目录名，并保留块数、时间、owner 和权限语义。
- FTP 数据流现在会独立累计 WFP `missedBytes`，出现 missed bytes 时仍继续检查当前可用 stream payload，并在传输完成/失败语义中输出 `missedBytes`，不再静默跳过整次回调。
- 策略 IOCTL 的载荷容量已从错误的 4096 字节提升到 16384 字节，并在 kernel/native 两侧加入 `PS_POLICY_DATA` 编译期容量断言；此前策略结构已超过旧容量，保存时存在越界复制风险。
- FTP 现在有独立的命令、路径和内容规则族。`RETR/STOR/APPE/STOU` 数据按 512 字节分段生成 `FTP DATA_CONTENT` 语义，保留 63 字节滚动尾部用于跨 buffer 字符串匹配，并识别 PDF/ZIP/PNG/JPEG/GZIP/PE/OLE/JSON/文本等基础 MIME；目录项名称和传输参数由 FTP 路径规则独立匹配。
- 大策略结构不再作为局部变量压入 WFP/文件回调的内核栈；查询路径改用 NPAGED lookaside 快照，降低 classify 调用链的栈耗尽风险。
- 被动模式下，即使数据 socket 在 `LIST`/`NLST` 之前建立，也会把数据连接从建链命令（`PASV`/`EPSV`）重新绑定到后续传输命令；数据流查找也不再混淆“连接建立方向”和“当前 payload 方向”。
- HTTP stream parsing now keeps per-flow request/response reassembly buffers and tracks `Content-Length` and chunked-body state across buffers, not just within a single callback buffer.
- Stream inspection keeps per-flow dedup state using `flowHandle`, reducing duplicate HTTP/SNI/FTP audit events for the same connection.
- Stream inspection can drop a connection when the parsed HTTP/SNI/FTP event resolves to `ActionBlocked`.
- The shared policy model includes independent `blockedDomains` and `blockedUrls` fields.
- The shared policy model includes dedicated `blockedHttpHeaders` and `blockedHttpTrailers` fields.
- The shared policy model includes dedicated `blockedHttpBodyPatterns`.
- The shared policy model includes dedicated `blockedJsonKeys`, `blockedJsonPaths`, and `blockedJsonValues`.
- The shared policy model also includes dedicated `blockedFtpCommands`.
- Kernel/native/Electron plumbing supports `blockedDomains` and `blockedUrls`.
- Kernel/native/Electron plumbing supports `blockedFtpCommands`.
- Kernel/native/Electron plumbing supports `blockedHttpHeaders` and `blockedHttpTrailers`.
- Kernel/native/Electron plumbing supports `blockedHttpBodyPatterns`.
- Kernel/native/Electron plumbing supports `blockedJsonKeys`, `blockedJsonPaths`, and `blockedJsonValues`.
- The renderer policy panel exposes basic editors for `blockedDomains`, `blockedUrls`, `blockedFtpCommands`, `blockedHttpHeaders`, `blockedHttpTrailers`, `blockedHttpBodyPatterns`, `blockedJsonKeys`, `blockedJsonPaths`, and `blockedJsonValues`.
- Network policy queries consider blocked ports, blocked process names, blocked domains, blocked URLs, and blocked FTP commands.
- HTTP request/response semantic strings now include selected header fields, and chunked trailers are extracted into semantic strings before policy evaluation.
- HTTP policy evaluation can now block on configured header rules and trailer rules in addition to domain/URL rules.
- For `Content-Type: text/*` and `application/json`, HTTP body previews are now reassembled across buffers and evaluated against `blockedHttpBodyPatterns`.
- For `application/json`, body previews now produce structured `JSONKEY/JSONPATH/JSONVAL` semantics and can be blocked by dedicated JSON key/path/value rules.
- User-mode best-effort `gzip` / `deflate` decoding is now applied to HTTP body preview events before they are mirrored into the realtime audit chain.
- File-side quarantine redirection is now implemented for blocked create-like operations, and blocked writes on already-open handles now fall back to a stream-handle-context copy-on-write path that copies the original file into quarantine once and diverts subsequent blocked writes into the quarantine file instead of the original target.
- File-side quarantine now preserves original-path semantics more explicitly: file events carry both original and quarantine paths, redirected create handles receive a post-create stream-handle context so later name/query/read/write operations can still expose the original path view while using the shadow file state underneath, and Electron audit details now show `original | QUARANTINE shadow` instead of collapsing everything to the shadow path alone.
- Electron main now hosts a real local HTTP proxy / HTTPS CONNECT MITM chain with root-CA generation/import, per-host leaf certificate export, request/response inspection, optional system-proxy switching hooks, HTTP/2 upstream/downstream support, WebSocket upgrade forwarding, and connection-level process attribution restored via native TCP owner lookup.
- `PreWrite` is now explicitly paged like the other file callbacks, and synthetic quarantine-write paths now refuse to run unless they are executing at `PASSIVE_LEVEL`.
- File/net ring buffers now align their usable capacity to whole event-slot boundaries, expose dropped-event counters, and surface queue depth / drop counts through `DRIVER_STATUS`.
- File/net event delivery now supports fixed-size batch dequeue from kernel to user mode, and the user-mode event loop now prefers `waitForEvents()` + batch drain over fixed-interval busy polling.
- The in-memory DLP event surface is now backed by a local durable queue in Electron main: startup replays persisted events from snapshot+journal, new events append to disk, shutdown compacts the queue, and clearing audit state also clears the local queue.
- The local durable event queue now also behaves like a resumable spool: each persisted event carries `pending/sent/failed` state plus retry metadata, journal replays state transitions, and Electron main exposes lease/ack/fail IPC hooks for downstream delivery workers.
- A first local delivery worker now exists in Electron main: it periodically leases `pending/failed` events from the durable queue, appends successful deliveries to a local delivered ledger, marks success with ACK metadata, marks failures with retry metadata and backoff, and exposes runtime status over IPC.
- The first real `EventSink` implementation now exists locally: the delivery worker can persist configurable HTTP sink settings, batch POST leased events to a local/remote HTTP endpoint, ACK successful batches, and roll failed batches back into the spool with retry metadata.
- A local delivery test sink and end-to-end validation path now exist: Electron main can start a loopback HTTP sink, the delivery worker can post queued events into it, and the installed/self-check path can verify that queued events transition from `pending` to `sent` with delivered receipts written locally.
- `EventSink` is now formally abstracted on the sender side: the delivery worker no longer hardcodes HTTP delivery logic and instead resolves pluggable sink implementations (`http`, `local-test`) behind a common interface; end-to-end delivery validation still passes after this refactor.
- The local durable queue now has finer-grained delivery state: leased events move into `sending`, retryable failures stay in `failed` with backoff metadata, and non-retryable/exhausted events are moved into `abandoned` with dead-letter records instead of being retried forever.
- Delivery ACK handling is no longer batch-wide only: sink responses can now carry per-event `ack/retry/drop` outcomes, the worker applies them item by item, and partial success no longer rolls the whole batch back into the queue.
- A real center-oriented sink now exists beside the generic HTTP sink: `central-http` adds node/tenant identity, bearer-token style auth headers, per-event server ACK parsing, and a loopback self-check path that proves `allow + retry-once + drop` behavior end to end.
- Event delivery is now pipeline-based instead of single-sink-only: the worker can fan one leased batch out to multiple enabled sinks under `all` or `any` strategy, and the installed self-check now proves dual-sink fan-out (`http` + `central-http`) end to end.
- Center-issued backpressure is now consumed by the delivery worker: sink responses can advertise `pauseMs` and `maxBatchSize`, the worker keeps per-sink pause windows, and subsequent leases honor those pause/batch limits instead of continuing to push at a fixed rate.
- The center sink now supports stronger trust plumbing: outbound HTTPS delivery can load custom CA/client cert/key material for mTLS-style deployments, requests can carry HMAC signatures, and center receipts can be signature-verified before the batch is accepted locally.
- Dead-letter handling is no longer write-only: abandoned events are still journaled to `dead-letter.ndjson`, and Electron main now exposes a basic abandoned-event requeue path so operators can replay dead letters without manual queue surgery.
- JSON body path rules are no longer limited to raw substring matching: kernel-side HTTP inspection and user-mode MITM/WebSocket JSON inspection now understand wildcard-style path patterns such as `users[*].password`, `users[].password`, and broader `*` glob segments against emitted JSON paths.
- JSON path rules now also support a lightweight sibling-predicate form on both kernel and user-mode inspection paths, using a syntax such as `users[*]{role=admin}.password` to express "match this descendant path only when the surrounding object has a sibling field/value condition".
- JSON predicate rules now support multiple sibling conditions joined by `&&` (and `,` as a compatibility separator) on both kernel and user-mode inspection paths, for example `users[*]{role=admin&&status=active}.password`.
- JSON predicate rules now also support `!=` negation and `||` alternatives inside the sibling-condition block on both kernel and user-mode inspection paths, so expressions such as `users[*]{role=admin&&status!=disabled||role=auditor}.password` are now valid.
- JSON predicate rules now also support integer-style numeric comparisons with `>`, `>=`, `<`, and `<=` inside the sibling-condition block on both kernel and user-mode inspection paths, for example `orders[*]{amount>=1000&&classification=secret}.payload`.
- Multi-sink retries no longer blindly resend to sinks that already ACKed a given event: each persisted queue item now carries per-sink delivery state, the worker only re-delivers to sinks still in `pending/retry`, and the self-check now shows the second pass hitting only the center sink instead of duplicating the already-acked HTTP sink.
- Electron main now also exposes basic queue-observability hooks for delivery operations: recent delivered/dead-letter records can be queried, and the durable queue view can be listed by `sent` / `abandoned` state without opening raw NDJSON files by hand.
- Startup recovery for the durable queue is now stricter: events found in `sending` after process restart are reclaimed back into retryable state instead of sitting inflight until an old lease timeout elapses.
- Sink backpressure windows are now persisted locally and reloaded on restart, with expired pause entries pruned automatically so a transient center throttle survives a restart but does not stick forever.
- Delivery now keeps a separate durable batch-checkpoint ledger alongside the event queue: each batch records per-sink stage (`sending/completed/failed/dropped`) plus affected event IDs, so crash recovery can rebuild sink-level progress even if the main queue item was not fully updated before the process died.
- The renderer now has a first delivery-operations panel under audit: operators can see worker status, pipeline/backpressure, batch checkpoints, dead-letter queue, recent delivered/dead-letter records, and can trigger replay / clear-backpressure / clear-completed-checkpoint actions without dropping to raw IPC or filesystem inspection.
- Checkpoint handling now also has a basic archive path: completed/recovered checkpoints that age out or are manually cleared are appended into a local archive ledger instead of being silently dropped, giving operators a minimal historical trail beyond the hot checkpoint set.
- Delivery operations now include first-class pipeline config editing/history and report export: the renderer can edit the pipeline JSON, persist a config-change history, show archived checkpoints, and trigger a local JSON export of the current delivery operational state for troubleshooting.
- Network policy evaluation no longer does heavyweight URL/domain/header/body string scans while holding the global policy spin lock; it snapshots policy first and matches outside the lock.
- Process tracking now keeps exited processes as tombstones for a grace window instead of freeing them immediately, which preserves late-arriving attribution for file/network events after process exit.
- The device/IOCTL path now uses `IO_REMOVE_LOCK` and stops accepting new requests before teardown, which closes the main unload-vs-IOCTL race the earlier analysis pointed out.
- File-side consistency is now improved for existing quarantined handles: `IRP_MJ_READ` can synthesize reads from the shadow file for copy-on-write handles, and `IRP_MJ_QUERY_INFORMATION` can answer selected metadata classes from the quarantine context instead of the original file object.
- Directory enumeration now has a real post-op rewrite pass: for supported `IRP_MN_QUERY_DIRECTORY` information classes, entries with known original->quarantine mappings are rewritten to the shadow file's metadata view instead of exposing the original file's size/timestamps.
- A first complete WFP `ALE_CONNECT_REDIRECT` chain now exists: redirect handle lifecycle, redirect settings IOCTL, `(processId + local endpoint) -> original destination` mapping, kernel redirect classify on V4/V6 connect-redirect layers, native query bridge, and local proxy transparent ingress now form a usable "kernel diversion -> user-mode MITM -> original destination recovery" path.
- Redirect mappings are no longer just TTL-based cache entries: `ALE_FLOW_ESTABLISHED_V4/V6` now binds matching redirect entries to `flowHandle`, and `flowDeleteFn` removes them on flow teardown; TTL remains as a fallback for unbound/stale entries.
- WebSocket upgrade handling in the local proxy is no longer a blind tunnel: after `101 Switching Protocols`, the proxy now parses WebSocket frames, reassembles fragmented text messages, evaluates text/json message content against the existing body and JSON policy families, and sends `1008 Policy Violation` close frames when a rule is hit.
- WebSocket `permessage-deflate` is now partially supported in the local proxy: if the upgraded session negotiates `permessage-deflate` together with `client_no_context_takeover` / `server_no_context_takeover`, the proxy inflates completed compressed text messages before applying body/JSON rules; sessions without `no_context_takeover` are currently forwarded without content inspection to avoid false decoding.
- Validation status on the current machine:
  - kernel/native/Electron builds pass
  - transparent redirect end-to-end validation is currently blocked by lack of administrator privilege to load the driver in the validation runner
  - explicit HTTP proxy validation succeeds and emits the expected ingress/request logs
  - explicit HTTPS validation reaches `CONNECT 200` and TLS ingress logging, but the decrypted HTTPS request does not yet complete through the current transparent/MITM ingress path, so HTTPS transparent ingress still needs follow-up work
- WFP initialization failure is a hard failure for driver load instead of silently degrading to file-only protection.
- Network events populate `timestamp`, `timestampMs`, `processName`, and process attribution fallback.
- File events preserve `ActionLogged` instead of collapsing everything into `allowed`.
- Process tracking uses process create/exit callbacks and removes stale entries on process exit.
- `PID 0` and `PID 4` are normalized to `System`.
- For already-running processes, process-name fallback prefers the real image path basename via `SeLocateProcessImageName` before falling back to `PsGetProcessImageFileName`.
- File write events populate `fileSize` with the write length.
- Electron polling uses kernel-derived timestamps instead of stamping everything with `new Date()`.
- Native audit modules are mirrored from the real-time polling path, so audit storage and real-time events are no longer disconnected.
- Policy saves now cross the Electron IPC boundary as plain DTOs instead of Vue reactive proxies; file audit paths are normalized from NT device paths to DOS drive paths in native user mode, and PersonalSafer's own user-data/ProgramData writes are excluded to prevent the durable audit queue from recursively auditing itself and crowding out network events. Audit views refresh continuously and classify HTTP/FTP/SNI/WebSocket records as network events.
- Policy updates now require a successful kernel readback before the renderer reports success; the UI is replaced with the applied policy, reports fields normalized/truncated by kernel limits, preserves blocked extensions on readback, exposes blocked-port editing, and no longer presents action selectors that were not represented by the kernel policy ABI.
- File quarantine now has a recoverable user-mode catalog/index. Quarantine events upsert original/shadow path, process, event/action, timestamps and occurrence count into snapshot+journal storage; IPC and the DLP page expose searchable records with live file presence and size, and a two-process Electron validation proves restart recovery.
- Policy configuration is now product-persistent instead of kernel-memory-only: a versioned atomic `config/policy.json` store validates every rule against kernel ABI capacities, preserves corrupt files for diagnosis, rejects invalid/duplicate rules before IOCTL, supports offline desired-policy saves, replays the desired policy with kernel readback after manual or packaged driver load, and starts the packaged local proxy as part of the same startup closure. A restore failure unloads the driver instead of leaving default policy active behind a false healthy state; proxy failure is reported without stopping file-event polling. Policy and quarantine stores both pass two-process restart validation.
- Clearing audit logs also clears the in-memory polling buffer.
- Stale `src/kernel/comm/ioctl_handler.*` code was removed from the repo.
- Kernel build verification works on the local machine with the installed WDK at `D:\Windows Kits\10`.
- 已收敛一组可直接导致驱动蓝屏的内核栈/IRQL 风险：WFP ALE、HTTP/SNI/FTP inspection 与 MiniFilter create/read/write 不再把 3.6 KB 到 5.6 KB 的事件结构放在内核栈上，统一改用 NonPaged lookaside；HTTP 重组不再使用 8 KB 栈临时缓冲；FTP 响应不再复制超过 20 KB 的完整 flow cache 到栈；WFP 高 IRQL 路径不再进入基于 `FAST_MUTEX` 的进程缓存查询。
- WFP teardown 顺序已改为先移除动态 filters 并注销 callouts，等待回调退出后再销毁 redirect handle 和目的地映射，避免卸载期间回调使用已销毁 redirect 资源。
- 2026-07-15 `fltmc load PersonalSafer` 蓝屏已由完整转储实证：`BugCheck D1` 发生在 WFP stream receive 的 `DISPATCH_LEVEL`，`PersonalSafer+0x13196` 是 MSVC `_chkstk` 栈探测，调用链 `ProcessHttpStreamState (+0xA98B) -> InspectStreamPayload (+0xE44F) -> NETIO`；被访问地址位于 DPC 栈底页边界。根因是 HTTP 重组和事件对象造成的内核栈耗尽，不是普通悬空指针或签名问题。
- 蓝屏修复后的 PDB 栈帧基线为：`InspectStreamPayload=184 B`、`ProcessHttpStreamState=248 B`、`TryEnqueueFtpDataPreviewEvent=168 B`、`UrlContainsMatchingJsonPredicatePath=80 B`。2 KB 级 HTTP semantic helper 已强制禁止内联，WFP stream 可达路径不再包含 4 KB 以上栈帧；分类缓存改在 `DriverEntry` 的 PASSIVE_LEVEL 显式初始化，不再由首个 DPC 回调懒初始化。
- 内核栈检查已扩展为全 PDB 门禁：`scripts/verify-kernel-stack-frames.ps1` 解析 Release PDB 的全部 `S_FRAMEPROC`，项目函数上限为 1024 B，构建缺少 PDB/`llvm-pdbutil` 或任一项目函数超限都会失败。当前 238 个函数帧通过；最大项目帧为 `BuildQuarantinePath=720 B`。链接进驱动的 MSVC CRT `_woutput_l=1168 B` 采用独立 1280 B 上限。
- 文件隔离和网络内容检查中的大 scratch 已继续迁移到 heap：HTTP header/trailer/body/JSON semantic 直接在 NonPaged 输出缓冲构造，FTP content/response/SNI/directory parser 使用 NonPaged scratch，文件 create redirect/COW context/directory post-op 使用 PASSIVE_LEVEL Paged scratch。
- JSON body parser 的递归累计栈也已收敛：最大深度仍受 8 层限制，但每层 key/path/value 数组已迁移到一次性 NonPaged parse scratch，递归帧只保留标量和指针，避免“单帧合规但多层累计超限”。
- 安装包驱动漏包已修复：Windows staging 中遗留的 `personalsafer.sys` 小写文件名不会再因 electron-builder 大小写敏感 filter 被排除。`copy-native` 现在先删除大小写不规范的旧目标再复制为 `PersonalSafer.sys`；完整构建会清理陈旧 `win-unpacked`，并在 NSIS 生成后校验 packaged driver 的存在性和 SHA256，缺失或哈希不一致会直接使构建失败。
- “先添加 443 端口、再编辑文件后缀”导致界面未响应的问题已按事件风暴链修复：内核确认的 PersonalSafer 产品进程不再被普通网络端口策略自阻断，避免本地代理/投递 worker 在 443 上反复失败；事件轮询每轮 drain 数量受限，并在高流量下主动让出 Electron 主事件循环；策略标签编辑改为不可变数组替换，避免复用嵌套响应式代理。
- DLP 默认豁免已统一到内核公共入口：PersonalSafer 自身 PID、`PID 0/4`，以及映像位于 `\SystemRoot\` / `\Windows\` 的 Windows 系统进程不进入文件/网络策略，不隔离且不写文件/网络审计队列；安装目录和驱动文件的自保护仍独立生效。第三方 LocalSystem 服务若映像不在 Windows 目录不会自动豁免。进程退出会同时清除产品/动态/系统 PID 缓存，避免 PID 重用导致误豁免。
- 安装程序在驱动启动后卡住的问题已按安装/自保护生命周期修复：NSIS 不再在完成页自动启动应用；升级和卸载初始化阶段会先调用旧安装程序的固定令牌维护入口解锁，再结束旧 UI、卸载 MiniFilter并停止服务。若服务仍未停止则明确中止并提示重启，不再带着锁定驱动继续覆盖安装目录。自定义 NSIS hook 已纳入构建并通过编译。

- 2026-07-16 桌面快捷方式“点击无反应”已修复：主窗口关闭后实际隐藏到托盘，第二实例原先只调用 `focus()`，无法恢复隐藏窗口。单实例回调现在依次执行 `show()`、`restore()`、`moveTop()` 和 `focus()`。自动回归已验证窗口从隐藏状态再次启动后恢复可见，主进程保持响应。
- 2026-07-16 针对“驱动加载后桌面点击卡顿/无响应”收敛文件 I/O 热路径：系统和 PersonalSafer 进程现在会在事件分配、规范化文件名查询和策略匹配前直接豁免；驱动加载前已存在的进程只在首次文件操作时解析一次完整镜像路径，不再对每次 I/O 调用 `SeLocateProcessImageName`；Release 驱动不再启用 `DbgPrintEx`，事件环形队列满时也不再逐条打印丢弃日志。内核 Release 构建、253 个 PDB 栈帧门禁、签名和安装包驱动哈希校验通过，仍需在 VM 加载驱动后完成 Explorer 响应对比实证。
- Explorer 无响应进一步确认了 MiniFilter 初始化竞态：旧顺序在 `ProtectInitialize` 之前调用 `FltStartFiltering`，Explorer 可能先进入进程缓存，但因保护上下文尚未初始化而没有被标记为 Windows 系统进程，之后会持续执行约 13 KB 策略快照复制、路径规范化和审计入队。DriverEntry 现在先初始化保护、进程跟踪、事件、策略、分类和 WFP，最后才启动 MiniFilter；卸载时先注销 MiniFilter/WFP 回调，再清理保护和策略状态。修复后内核 Release 构建和 254 个 PDB 栈帧门禁通过，最终 VM 实证应确认审计中不再出现 `explorer.exe` 文件事件。
- 域名规则命中后浏览器、产品进程和桌面连锁卡死的 WFP 根因已修复：stream filters 原注册为 `FWP_ACTION_CALLOUT_INSPECTION`，却用仅对 `FWP_ACTION_CALLOUT_UNKNOWN` 有效的 `FWPS_STREAM_ACTION_DROP_CONNECTION`，导致命中后连接没有真正终止并持续等待/重试。V4/V6 stream filters 已改为 UNKNOWN callout；阻断覆盖当前完整 stream buffer、设置 DROP_CONNECTION/ABSORB 并清除 action-write 权限；系统和 PersonalSafer 流量在 payload 复制及 HTTP/SNI/FTP 解析前直接放行。VM 验证标准是域名命中后浏览器快速连接失败、系统保持响应且每个连接只产生有限阻断事件。
- 安装/卸载旧版本时重命名驱动卡死的生命周期缺陷已修复：NSIS 不再无期限等待维护解锁、`taskkill`、`fltmc unload` 或 `sc stop`，每个外部调用都有 10-20 秒超时；覆盖文件前会再次查询 `fltmc filters`，只有明确确认 PersonalSafer MiniFilter 已消失才继续。服务状态不再作为可重命名驱动的充分条件；超时、状态查询异常、维护程序缺失或 filter 残留都会在任何受保护文件变更前中止并提示重启。

## Remaining issues

- 内核自保护已按产品要求简化为单一 `IOCTL_PS_PROTECT_CONTROL`：原生 `connect()` 首次通信携带编译期固定令牌，自动下发安装目录、主程序映像、当前 PID 和系统驱动文件路径并立即锁定；维护删除只需同一 IOCTL 切换 `UNLOCK/RELOCK`，解锁窗口最长 300 秒。旧 challenge、RSA 签名解锁和显式清单 IOCTL 已停止分发。
  Files: `src/kernel/core/protect.c`, `src/kernel/device.c`, `src/kernel/filter/fltmgr.c`, `src/kernel/driver.c`, `src/native/src/dlp/kernel_comm.cpp`, `scripts/self-protection-maintenance.js`

- 自保护维护协议已接入安装版隐藏启动参数，VM 无源码时可直接用安装后的 EXE 执行状态查询、固定令牌解锁和回锁，并生成 JSON 结果；正常连接自动初始化保护目标，不再接受证书、私钥、路径或 PID 参数。
  Files: `src/electron/main/modules/protection_maintenance.ts`, `src/electron/main/index.ts`

- 自保护的进程清单由首次 bootstrap 自动建立为“当前 PID + 当前主程序映像路径”；进程创建/退出通知动态维护匹配映像的 PID。首次成功 bootstrap 固定安装目录、驱动和映像目标，后续相同映像连接只补 PID，不能覆盖目标；bootstrap 前禁止解锁。
  Files: `src/kernel/core/protect.c`, `src/kernel/filter/process_tracker.c`, `src/native/src/dlp/kernel_comm.cpp`

- 驱动服务注册表树已接入 `CmRegisterCallbackEx`，兼容注册表对象名解析后的实际 `ControlSetXXX` 路径；锁定态阻止服务键删除、值修改、改名、键信息和安全描述符修改，解锁后允许标准卸载。
  Files: `src/kernel/core/protect.c`

- 设备访问边界采用兼容性收敛：普通事件/状态读取继续使用现有设备；单一保护控制 IOCTL 使用编译期固定令牌。真正的设备 DACL 最小化仍依赖后续 SYSTEM broker 独占控制句柄和受限客户端 IPC，固定令牌只防误调用，不对抗逆向或本机管理员。
  Files: `src/kernel/device.c`, `src/electron/main/modules/protection_maintenance.ts`

- File quarantine now covers both create-like landing paths and blocked writes on existing handles, preserves original/quarantine path identity, and maintains a recoverable searchable catalog with live shadow-file status. Guarded restore/purge workflows are available by catalog ID: the protected product process enters maintenance mode, restores or deletes the registered shadow, persists the result, and always relocks. Remaining gaps are full filesystem fidelity for exotic oplock/append/section-backed cases and cryptographic evidence manifests.
  Files: `src/kernel/filter/fltmgr.c`, `src/kernel/policy/policy_engine.c`

- The user-mode local proxy / MITM path now supports HTTP/1.1, HTTP/2, and WebSocket forwarding with connection-level process attribution for decrypted HTTPS requests, and it now inspects post-upgrade WebSocket text/json messages with `permessage-deflate` support for `no_context_takeover` sessions, but it still buffers full request/response bodies in user mode, does not yet implement richer binary-frame semantics or context-takeover compression inspection, and does not provide a true kernel-grade process identity chain once traffic is multiplexed or re-used above the socket layer.
  Files: `src/electron/main/modules/local_proxy.ts`, `src/electron/main/ipc/handler_registry.ts`

- Event delivery now has checkpoint archival, UI-side pipeline editing/history, and exportable delivery reports, but it still lacks richer retention policy controls for hot/archive checkpoint tiers, does not yet diff/validate sink config changes semantically before save, and exported reports are still snapshot JSON artifacts rather than richer signed/bundled operational evidence packages.
  Files: `src/electron/main/modules/delivery_worker.ts`, `src/electron/main/modules/event_sink.ts`, `src/electron/main/modules/event_sink_central_http.ts`, `src/electron/main/modules/dlp_events.ts`, `src/electron/main/ipc/handler_registry.ts`, `src/electron/renderer/src/components/audit/DeliveryOperations.vue`

- FTP 已能关联主动/被动数据通道、传输生命周期、已检查/遗漏字节总量，结构化 Unix/DOS/EPLF/VMS `LIST`、`NLST`、`MLSD` 目录项，以及独立的命令/路径/明文内容规则；剩余缺口是少数厂商私有 LIST 变体、恢复 WFP 已遗漏 payload 的内容语义，以及 PDF/Office/压缩包等文件格式的深层内容解码。
  Files: `src/kernel/network/classification.c`

- HTTP keep-alive / pipelining is now handled across buffers with header/body state, chunked-body progression, trailer termination, header/trailer/body semantic extraction, lightweight structured JSON inspection, and best-effort user-mode gzip/deflate preview decoding, but the implementation still does not do full in-kernel compression-aware body parsing, upgrade/tunnel semantics, or a full RFC-grade edge-case reassembly engine.
  Files: `src/kernel/network/classification.c`

- The payload policy model now supports dedicated HTTP domain/URL/header/trailer/body, FTP command/path/content, and JSON key/path/value rules, plus wildcard-style JSON path matching including array wildcards and multi-condition sibling-predicate syntax with `&&`, `||`, `!=`, and integer-style numeric comparisons, but it still does not expose parentheses/precedence control, decimal-number comparisons, deeper arbitrary predicate expressions, or user-mode regex evaluation for FTP transferred content.
  Files: `src/kernel/common/shared_types.h`, `src/kernel/policy/policy_engine.c`, `src/native/src/dlp/policy_manager.cpp`
