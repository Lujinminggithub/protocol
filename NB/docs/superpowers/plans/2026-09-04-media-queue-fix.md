# 直播媒体队列修复实施计划

> **供智能执行器使用：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 逐任务实施；所有步骤使用复选框跟踪。

**目标：** 在持续限速仍为 5Mbps 的前提下吸收手机短时 12Mbps 媒体突发，避免固定 256KiB 队列淘汰最旧 UDP 包造成观看画面断层，并让 UDP 队列年龄真实可见。

**架构：** 由 `nb_live` 根据目标速率和链路重排序窗口计算媒体队列容量，由 `nb_udp_queue` 读取带时间戳 datagram 队首年龄，节点队列代码统一使用动态容量和真实年龄。先热更新租户突发额度，再通过共享二进制发布代码修复。

**技术栈：** C11、picoquic、现有 NB 控制套接字、Python 部署工具、CMake/CTest。

**规格：** `docs/superpowers/specs/2026-09-04-media-queue-exit-recovery-design.md`

## 全局约束

- 上下行持续速率保持 5000Kbps。
- 上行突发额度为 2500000 字节，下行突发额度保持 625000 字节。
- `50000+` UDP 媒体路径继续经过广州2、香港2、哈萨克斯坦三跳。
- `signal_direct` 保持关闭。
- 媒体丢弃必须以完整原始 UDP 包为单位，不允许留下部分分片。
- 新增日志说明使用中文；机器可解析字段名保持英文。

---

### 任务 1：动态媒体队列容量

**文件：**
- 修改：`src/nb_live.h`
- 修改：`src/nb_live.c`
- 测试：`tools/test_live.c`

**接口：**
- 输入：流分类、目标速率 `target_rate_bps`、重排序窗口 `reorder_delay_us`。
- 输出：`size_t nb_live_udp_queue_limit(nb_flow_class_t flow_class, uint64_t target_rate_bps, uint64_t reorder_delay_us)`。

- [ ] **步骤 1：先写失败测试**

```c
assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_MEDIA,5000000,462000)==589824);
assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_MEDIA,0,462000)==262144);
assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_MEDIA,100000000,2000000)==1048576);
assert(nb_live_udp_queue_limit(NB_FLOW_CLASS_CTRL,5000000,462000)==262144);
```

- [ ] **步骤 2：运行红灯测试**

运行：`gcc -std=c11 -Isrc tools/test_live.c src/nb_live.c -o build/test_live_queue.exe`

预期：因 `nb_live_udp_queue_limit` 尚不存在而编译失败。

- [ ] **步骤 3：实现容量公式**

```c
size_t nb_live_udp_queue_limit(nb_flow_class_t flow_class,uint64_t rate,uint64_t reorder_us){
    if(flow_class!=NB_FLOW_CLASS_MEDIA||rate==0)return 256u*1024u;
    uint64_t window=reorder_us<200000?200000:reorder_us;
    uint64_t bytes=(rate/8u)*window/1000000u*2u;
    if(bytes<256u*1024u)bytes=256u*1024u;
    if(bytes>1024u*1024u)bytes=1024u*1024u;
    return (size_t)((bytes+65535u)/65536u*65536u);
}
```

- [ ] **步骤 4：运行绿灯测试**

运行：`gcc -std=c11 -Isrc tools/test_live.c src/nb_live.c -o build/test_live_queue.exe; ./build/test_live_queue.exe`

预期：退出码 0。

### 任务 2：真实 datagram 队列年龄

**文件：**
- 修改：`src/nb_udp_queue.h`
- 修改：`src/nb_udp_queue.c`
- 测试：`tools/test_udp_queue.c`

**接口：**
- 输入：现有 10 字节记录头格式的队列、队列长度和当前微秒时间。
- 输出：`uint64_t nb_udp_queue_oldest_age_us(const uint8_t *queue, size_t length, uint64_t now_us)`；空队列、损坏队列和未来时间戳返回 0。

- [ ] **步骤 1：先写失败测试**

在测试记录头中写入字面量时间戳 `1000`，断言当前时间 `6000` 时年龄为 `5000`；消费首包后断言年龄对应第二条记录；损坏长度返回 0。

- [ ] **步骤 2：运行红灯测试**

运行：`gcc -std=c11 -Isrc tools/test_udp_queue.c src/nb_udp_queue.c src/nb_udp.c -o build/test_udp_queue_age.exe`

预期：因新 API 不存在而编译失败。

- [ ] **步骤 3：实现只读队首年龄**

读取记录头偏移 2 的大端 64 位 `queued_at`，复用 `record_peek` 校验记录完整性，不移动队列。

- [ ] **步骤 4：运行绿灯测试**

运行同上并执行 `build/test_udp_queue_age.exe`，预期打印 `nb_udp_queue_test: ok`。

### 任务 3：节点接入动态容量和队列诊断

**文件：**
- 修改：`src/nb_node_udp_queue.inc`
- 修改：`src/nb_node_local.inc`
- 修改：`src/nb_node_core.inc`
- 修改：`src/nb_session.h`
- 测试：`tools/test_live.c`
- 测试：`tools/test_udp_queue.c`

**接口：**
- 消费任务 1 的 `nb_live_udp_queue_limit`。
- 消费任务 2 的 `nb_udp_queue_oldest_age_us`。
- 产出每条媒体流的动态硬上限、真实三方向队列年龄以及限频的 `media queue pressure` INFO 日志。

- [ ] **步骤 1：把 `dgramq_append`、`udp_queue_make_media_room` 和 optional FEC 入队改为显式接收 queue limit**

方向对应运行时：Entry/Middle 下行使用 `transport_egress`，Exit 回程使用 `transport_ingress`；缺少活动 profile 时传入零速率并回退 256KiB。

- [ ] **步骤 2：为 `proxy_stream_t` 增加 `udp_queue_pressure_last_warn_at`**

同一流最多每秒输出一次中文说明日志，字段包含 `role/id/target/dropped/queue_bytes/queue_limit/oldest_age_ms`。

- [ ] **步骤 3：修复日志和 metrics 的 UDP 年龄来源**

`ps_log_media_metrics` 和 `control_render(metrics)` 对 UDP 三方向调用 `nb_udp_queue_oldest_age_us`，不再固定返回 0 或读取 TCP ring 长度。

- [ ] **步骤 4：运行相关单元测试和差异检查**

运行：`python tools/test_shard_deploy.py`、两个 C 测试、`git diff --check`；预期全部退出 0。

### 任务 4：5Mbps 突发热更新与合成验证

**文件：**
- 新建：`build/deploy_candidate_media_burst.py`
- 验证：远端 `/etc/NB/instances/gz2-hk2-kz-00002_1/tenant.conf` 和两个 Entry 控制端点。

**接口：**
- 输入：`rate_up_kbps=5000`、`rate_down_kbps=5000`、`burst_up_bytes=2500000`、`burst_down_bytes=625000`。
- 输出：Entry/Exit 同一租户 generation 的 prepare/commit/readback 结果。

- [ ] **步骤 1：生成并校验新 tenant-v3 记录**

记录必须为 `tenant-v3 nbmobile 256 64 5000 5000 0 2500000 625000`。

- [ ] **步骤 2：按 Entry/Exit prepare 后 commit，失败恢复旧文件**

不重启 shard，不修改 sustained rate。

- [ ] **步骤 3：运行 12Mbps、500ms 突发探针**

采样前后 `queue_pressure_dropped`，预期增量为 0；持续 90 秒平均吞吐仍不超过 5Mbps 服务配置。

- [ ] **步骤 4：提交媒体队列实现**

仅暂存本计划涉及的源码、测试和运维脚本，提交说明使用中文：`修复：吸收直播媒体突发并暴露UDP队列年龄`。
