# 02 内核架构（单 sys 内部分层）

> 上级：[README](./README.md) ｜ 相关：[01 总体架构](./01-architecture-overview.md)、[03 异步裁决](./03-async-adjudication.md)

## 决策：单一 sys + 内部分层（ADR D1/D2）

保留单一 `PersonalSafer.sys`，理由：文件/网络/进程需强关联、共享策略与通信通道；内核态拆分买不到崩溃隔离（任一驱动 BSOD 整机蓝屏）。拆分的收益仅「独立编译/更新」，当前不值得。

真正要做的不是拆 sys，而是**收紧内部分层**：Core 共享层 + 可插拔 Providers。

## 目标内部结构

```
PersonalSafer.sys
├── DriverEntry / Unload 编排
├── Core 共享层  (不依赖任何具体 provider)
│   ├── policy      策略缓存 + 判定接口 + 版本号
│   ├── process     进程/身份追踪(PID→名/SID/签名)
│   ├── event       事件通道(环形→可靠管道)
│   ├── adjudicate  异步裁决队列(pend 登记/回填/超时)  → 见 03
│   └── protect     自保护状态机                        → 见 06
├── Providers  (只依赖 Core 接口)
│   ├── file  MiniFilter: PreCreate/PreWrite/PreSetInfo (+ 目标待补 READ/CLEANUP)
│   ├── net   WFP: ALE_AUTH_CONNECT/RECV_ACCEPT + STREAM (+ 待补 CONNECT_REDIRECT)
│   └── usbvol 可移动卷识别 + 设备控制                  → 见 04
└── comm  IOCTL / FltPort 分发(先于保护判断)
```

依赖方向单向：`Providers → Core`，Provider 之间不互相调用。当前目录 `filter/ network/ policy/ common/` 已接近此形状，需把 Core 边界从各处收敛。

## 服务/INF 结构（ADR D2）

- **一个 sys = 一个内核服务 = INF 里一个 `ServiceInstall`**，按 MiniFilter 规范注册。
- **WFP callout 不进 INF**：由驱动运行时 `FwpsCalloutRegister0` + `FwpmCalloutAdd0` 动态注册。INF 里没有 WFP 项是正常的，不是缺失。
- 当前 `inf/PersonalSafer.inf` 关键项（正确）：
  - `Class=ActivityMonitor`、`ServiceType=2`(FS filter)、`StartType=3`(demand)、`LoadOrderGroup="FSFilter Activity Monitor"`、`Dependencies="FltMgr"`、`Altitude=370030`。

### INF 已知问题（待修，不在本文档改）
1. Instances 子键语法错误（引号位置）：应为
   `HKR,"Instances\%Instance1.Name%","Altitude",0x00000000,%Instance1.Altitude%`
   （Flags 同理）。
2. `Instance1.Altitude` 在 [Strings] 中重复定义多次，应只留一行。
3. `StartType=3`(demand) 对当前架构是优点：由用户态服务按需启动，此时 BFE 通常已就绪，天然规避 WFP 的 BFE 依赖。

## 加载编排：WFP 对 BFE 的依赖（关键）

MiniFilter 由 FltMgr 管理、可较早注册；WFP 的 `FwpmEngineOpen0` 依赖 **BFE（Base Filtering Engine）服务就绪**。

- **当前**：demand-start，由用户态服务启动，BFE 一般已就绪 → 可直接注册。
- **若改开机即防护（boot/auto start）**：必须用 `FwpmBfeStateSubscribeChanges0` + `FwpmBfeStateGet0` 订阅，BFE 就绪回调里再注册 WFP callout；MiniFilter 部分照常早注册。
- 编排原则：**MiniFilter 早注册；WFP 延迟到 BFE ready；两者失败互不牵连**（一侧失败不应导致整驱动加载失败，除非策略要求）。

## 卸载编排

顺序（自保护解锁后才允许）：
1. 停 WFP：`FwpsCalloutUnregisterById0`（检查 `STATUS_DEVICE_BUSY` 并重试）+ `FwpmEngineClose0`。
2. 卸载 MiniFilter：`FilterUnload` 回调（受自保护状态机控制，见 [06](./06-self-protection.md)）。
3. 清理 Core：事件管道、进程表、策略——需 `IO_REMOVE_LOCK` 保证无在途 IOCTL/回调后再释放。

## 待办（与现状差距）

- [ ] 抽出 Core 层显式接口（policy/process/event/adjudicate/protect），Provider 只依赖接口。
- [ ] `driver.c` 加载编排显式区分 MiniFilter 与 WFP，补 BFE 就绪订阅（为将来 boot-start 铺垫）。
- [ ] 卸载路径引入 `IO_REMOVE_LOCK`；`FwpsCalloutUnregisterById0` 检查返回值。
- [ ] 修正 INF 三处问题。

## 涉及文件

- `src/kernel/driver.c`、`src/kernel/device.c`、`src/kernel/inf/PersonalSafer.inf`
- `src/kernel/filter/`、`src/kernel/network/`、`src/kernel/policy/`、`src/kernel/common/`
