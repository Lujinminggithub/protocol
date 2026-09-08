# NB-native YFE2 Relay-Exit FEC Implementation Plan

## 2026-09-08 执行检查点

- Tasks 1-9：实现、单元测试、Linux 严格构建和三跳回归已完成。
- Task 10 本地门禁：47 项 CTest、Go 源码包测试、canary/netem/snapshot Python 测试已完成。
- Task 10 兼容部署：真实 HK-108 Middle 与 KZ Exit 已部署 `4fc96af6b87669f3`；Entry 保持原版本；schema 1 TCP/UDP probe 通过。
- Task 10 worker 0 canary：协商 accepted；约 3.3Mbps 无损 baseline 开销 7.6%；0.2% 定向丢包进入 BURST，Exit 恢复 17 包；5 秒后回落 baseline；probe parity 不增长；worker 1 保持 schema 1/disabled；netem 已清理。
- 尚未完成：完整随机/突发/乱序/policer/app-limited/rate-cap 矩阵、30 分钟生产无损观察、worker 1 扩面。未达到这些门禁前不得扩面。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional, negotiated NB-native `NBUF v3` Reed-Solomon protection for Relay -> Exit media UDP, using baseline `16+1`, adaptive `16+3`, interleave, asynchronous parity generation, bounded recovery, probe isolation, and non-blocking fallback.

**Architecture:** Standard `NBUD v1` systematic datagrams remain unchanged and are queued immediately. A schema-2 Middle egress negotiates the built-in profile over an ordinary reserved `NBUD` control flow; after exact acceptance, sealed interleaved blocks are encoded by a bounded worker and emitted as low-priority `NBUF v3` parity. Exit supports v2 and v3 decoding, while every FEC failure degrades only the current connection to standard NBUD.

**Tech Stack:** C11, picoquic, pthread/eventfd, OpenSSL SHA-256, CMake/CTest, Go control-plane profile renderer, Python deployment/netem tools

**Spec:** `docs/superpowers/specs/2026-09-08-nb-native-yfe2-relay-exit-fec-design.md`

## Global Constraints

- Preserve the dirty working tree; never reset, revert, or commit pre-existing unrelated changes.
- For files already dirty at task start, stage only exact task hunks; never use whole-file `git add` on those files.
- Keep `NBUD v1` systematic wire bytes unchanged.
- Protect only Middle egress -> Exit ingress media UDP; Entry -> Middle, Exit -> Middle, TCP, and schema-1 behavior remain unchanged.
- FEC is optional and must never become an open-line, health, bandwidth, or rollback hard gate.
- `profile_id` is computed from canonical wire fields and must equal decimal `28909` (`0x70ed`).
- Product accounting and tenant limits count original business payload once; parity and recovered duplicates do not consume product quota.
- Original packets are queued before FEC work; worker, memory, encode, PMTU, or parity-queue failure may drop parity only.
- All resource bounds from the spec are hard limits, not advisory values.
- New log or UI rules use Chinese text; protocol identifiers and structured reason codes remain stable ASCII.

---

### Task 1: Canonical Profile and Schema-2 Transport Configuration

**Files:**
- Create: `src/nb_yfe2_profile.h`
- Create: `src/nb_yfe2_profile.c`
- Create: `tools/test_yfe2_profile.c`
- Modify: `src/nb_transport_profile.h`
- Modify: `src/nb_transport_profile.c`
- Modify: `tools/test_transport_profile.c`
- Modify: `controlplane/internal/transportprofile/profile.go`
- Modify: `controlplane/internal/transportprofile/render.go`
- Modify: `controlplane/internal/transportprofile/profile_test.go`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `nb_yfe2_profile_t`, `nb_yfe2_default_profile()`, `nb_yfe2_profile_validate()`, `nb_yfe2_profile_render()`, `nb_yfe2_profile_id()`.
- Produces: `nb_transport_link_profile_t.udp_fec_mode`, accepting `off`, legacy `adaptive-v2`, or `nb-yfe2-optional`.
- Produces: Go `LinkProfile.UDPFECMode string` and schema-2 rendering for Middle egress.

- [ ] **Step 1: Add failing canonical-profile tests**

```c
nb_yfe2_profile_t profile;
nb_yfe2_default_profile(&profile);
CHECK(profile.data_shards == 16 && profile.parity_shards == 1);
CHECK(profile.burst_parity_shards == 3 && profile.interleave == 4);
CHECK(profile.flush_ms == 10 && profile.recovery_deadline_ms == 150);
CHECK(profile.loss_trigger_packets == 1 && profile.hold_ms == 5000);
CHECK(nb_yfe2_profile_id(&profile) == 28909);
CHECK(nb_yfe2_profile_render(&profile, text, sizeof(text)) > 0);
CHECK(strcmp(text, "codec=nb-yfe2;wire_version=3;direction=relay_to_exit;data_shards=16;parity_shards=1;burst_parity_shards=3;interleave=4;flush_ms=10;recovery_deadline_ms=150;loss_trigger_packets=1;hold_ms=5000") == 0);
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: `cmake --build build --target nb_yfe2_profile_test && ctest --test-dir build -R '^nb_yfe2_profile$' --output-on-failure`

Expected: compile failure because `nb_yfe2_profile.h` and its functions do not exist.

- [ ] **Step 3: Implement the canonical profile API**

```c
typedef struct {
    uint8_t wire_version, data_shards, parity_shards, burst_parity_shards, interleave;
    uint16_t flush_ms, recovery_deadline_ms, hold_ms;
    uint64_t loss_trigger_packets;
} nb_yfe2_profile_t;

void nb_yfe2_default_profile(nb_yfe2_profile_t* out);
int nb_yfe2_profile_validate(const nb_yfe2_profile_t* profile);
int nb_yfe2_profile_render(const nb_yfe2_profile_t* profile,char* out,size_t cap);
uint16_t nb_yfe2_profile_id(const nb_yfe2_profile_t* profile);
```

Use OpenSSL `SHA256()` over the exact ASCII rendering and decode the first two bytes big-endian. Reject any non-default wire-affecting value in `nb_yfe2_profile_validate()`; runtime configuration may enable the built-in profile but may not customize it.

Link `nb_yfe2_profile_test` and `nb_node` with `OpenSSL::Crypto`; do not add another hashing dependency.

- [ ] **Step 4: Add failing transport schema tests**

```c
CHECK(load_profile("schema=1\n...\negress.udp_fec_mode=nb-yfe2-optional\n") != 0);
CHECK(load_profile("schema=2\nrole=middle\n...\negress.udp_fec_mode=nb-yfe2-optional\n") == 0);
CHECK(strcmp(loaded.egress.udp_fec_mode, "nb-yfe2-optional") == 0);
CHECK(load_profile("schema=2\nrole=entry\n...\negress.udp_fec_mode=nb-yfe2-optional\n") != 0);
CHECK(load_profile("schema=2\nrole=middle\n...\ningress.udp_fec_mode=nb-yfe2-optional\n") != 0);
```

- [ ] **Step 5: Implement schema-2 parsing without changing schema-1 defaults**

Allow top-level schema `1` or `2`. Schema 1 keeps the current `udp_fec_adaptive/k/hold` validation. Schema 2 accepts `udp_fec_mode`; only `role=middle` plus egress `nb-yfe2-optional` is valid. `off` is the default for absent schema-2 values.

- [ ] **Step 6: Add and pass Go renderer tests**

```go
if got.SchemaVersion != 2 || got.Egress.UDPFECMode != "nb-yfe2-optional" {
    t.Fatalf("new middle profile = %+v", got)
}
if strings.Contains(entryText, "nb-yfe2-optional") {
    t.Fatal("entry profile enabled relay-exit FEC")
}
```

Run: `go test ./controlplane/internal/transportprofile`

Expected after implementation: PASS.

- [ ] **Step 7: Run focused C tests and commit**

Run: `ctest --test-dir build -R '^(nb_yfe2_profile|nb_transport_profile)$' --output-on-failure`

Commit only Task 1 files with message: `feat: add canonical NB YFE2 profile`.

---

### Task 2: Optional `NBFC` Negotiation State Machine

**Files:**
- Create: `src/nb_yfe2_negotiation.h`
- Create: `src/nb_yfe2_negotiation.c`
- Create: `tools/test_yfe2_negotiation.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `nb_yfe2_profile_t` and profile ID from Task 1.
- Produces: fixed-size `NBFC v1` proposal/accept/reject codec.
- Produces: per-connection `nb_yfe2_negotiation_t` state with a 500ms optional timeout.

- [ ] **Step 1: Write failing negotiation wire tests**

```c
nb_yfe2_control_t proposal={.type=NB_YFE2_CTL_PROPOSE,.nonce=0x0102030405060708ULL};
nb_yfe2_default_profile(&proposal.profile);
CHECK(nb_yfe2_control_encode(wire,sizeof(wire),&proposal)==NB_YFE2_CONTROL_SIZE);
CHECK(nb_yfe2_control_decode(wire,NB_YFE2_CONTROL_SIZE,&decoded)==0);
CHECK(decoded.type==NB_YFE2_CTL_PROPOSE&&decoded.nonce==proposal.nonce);
CHECK(decoded.profile_id==28909);
wire[5]=0xff;
CHECK(nb_yfe2_control_decode(wire,NB_YFE2_CONTROL_SIZE,&decoded)!=0);
```

- [ ] **Step 2: Verify RED**

Run: `cmake --build build --target nb_yfe2_negotiation_test`

Expected: compile failure because negotiation types are missing.

- [ ] **Step 3: Implement fixed canonical control messages**

Define the 40-byte message exactly:

| Offset | Length | Field |
|---:|---:|---|
| 0 | 4 | magic `NBFC` |
| 4 | 1 | version 1 |
| 5 | 1 | type: `PROPOSE=1`, `ACCEPT=2`, `REJECT=3` |
| 6 | 1 | reason: `NONE`, `NOT_SUPPORTED`, `PROFILE_MISMATCH`, `PMTU`, `RESOURCE` |
| 7 | 1 | reserved zero |
| 8 | 8 | nonce, big-endian |
| 16 | 2 | Profile ID, big-endian |
| 18 | 1 | FEC wire version 3 |
| 19 | 1 | direction enum `RELAY_TO_EXIT=1` |
| 20 | 1 | K=16 |
| 21 | 1 | baseline R=1 |
| 22 | 1 | burst R=3 |
| 23 | 1 | interleave=4 |
| 24 | 2 | flush ms=10, big-endian |
| 26 | 2 | recovery deadline ms=150, big-endian |
| 28 | 8 | loss trigger packets=1, big-endian |
| 36 | 2 | hold ms=5000, big-endian |
| 38 | 2 | reserved zero |

Decoder rejects invalid type/reason combinations, nonzero reserved bytes, noncanonical Profile ID, or trailing bytes. `PROPOSE/ACCEPT` require reason `NONE`; `REJECT` requires a nonzero reason.

- [ ] **Step 4: Write failing optional-state tests**

```c
nb_yfe2_negotiation_start(&state, nonce, 1000000);
CHECK(state.state==NB_YFE2_NEG_PROPOSED);
CHECK(nb_yfe2_negotiation_timeout(&state,1499999)==NB_YFE2_NEG_PROPOSED);
CHECK(nb_yfe2_negotiation_timeout(&state,1500000)==NB_YFE2_NEG_FALLBACK_TIMEOUT);
nb_yfe2_negotiation_start(&state,nonce,2000000);
CHECK(nb_yfe2_negotiation_accept(&state,wrong_nonce,28909)!=0);
CHECK(state.state==NB_YFE2_NEG_PROPOSED);
CHECK(nb_yfe2_negotiation_accept(&state,nonce,28909)==0);
CHECK(state.state==NB_YFE2_NEG_ACCEPTED);
```

- [ ] **Step 5: Implement the state machine and reason rendering**

```c
typedef enum {
  NB_YFE2_NEG_DISABLED, NB_YFE2_NEG_PROPOSED, NB_YFE2_NEG_ACCEPTED,
  NB_YFE2_NEG_FALLBACK_TIMEOUT, NB_YFE2_NEG_FALLBACK_NOT_SUPPORTED,
  NB_YFE2_NEG_FALLBACK_PROFILE_MISMATCH, NB_YFE2_NEG_FALLBACK_PMTU,
  NB_YFE2_NEG_FALLBACK_RESOURCE
} nb_yfe2_negotiation_state_t;
```

State transitions are monotonic for one connection generation. Reject and timeout return fallback, never a connection-close result.

- [ ] **Step 6: Run and commit**

Run: `ctest --test-dir build -R '^nb_yfe2_negotiation$' --output-on-failure`

Commit: `feat: add optional NB FEC negotiation`.

---

### Task 3: `NBUF v3` Wire Codec

**Files:**
- Create: `src/nb_yfe2_wire.h`
- Create: `src/nb_yfe2_wire.c`
- Create: `tools/test_yfe2_wire.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Profile ID 28909 and existing `nb_udp_wire_view_t` descriptors.
- Produces: `nb_yfe2_parity_view_t`, `nb_yfe2_wire_encode()`, `nb_yfe2_wire_decode()`.

- [ ] **Step 1: Write a literal v3 round-trip test**

Build two descriptors with known sequence and lengths; encode parity index 16, K=16, R=1, actual count=2, shard size=1000. Assert the 24-byte header offsets literally, decode, and compare every descriptor and body byte.

- [ ] **Step 2: Add malformed-frame tests**

For separate fixtures mutate magic, version, unknown flag, Profile ID, direction, parity index, K, R, actual count, reserved byte, descriptor length, payload length, shard size, and total frame length. Each mutation must return a distinct `nb_yfe2_wire_error_t` value.

- [ ] **Step 3: Verify RED**

Run: `cmake --build build --target nb_yfe2_wire_test`

Expected: missing wire codec symbols.

- [ ] **Step 4: Implement the exact 24-byte header and 12-byte descriptors**

```c
#define NB_YFE2_WIRE_HEADER 24u
#define NB_YFE2_WIRE_DESC 12u
#define NB_YFE2_DATA_SHARDS 16u
#define NB_YFE2_BASE_PARITY 1u
#define NB_YFE2_BURST_PARITY 3u
#define NB_YFE2_FLAG_BURST 0x01u
```

Use explicit big-endian readers/writers and checked `size_t` arithmetic. The v3 decoder must not call or weaken the existing v2 decoder.

- [ ] **Step 5: Run and commit**

Run: `ctest --test-dir build -R '^nb_yfe2_wire$' --output-on-failure`

Commit: `feat: add NBUF v3 parity wire codec`.

---

### Task 4: Interleaved Block Builder and Immutable Encode Jobs

**Files:**
- Create: `src/nb_yfe2_block.h`
- Create: `src/nb_yfe2_block.c`
- Create: `tools/test_yfe2_block.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: source `nb_udp_wire_view_t`, v3 descriptor type, and `nb_rs_encode()`.
- Produces: four-way `nb_yfe2_tx_t`, immutable `nb_yfe2_encode_job_t`, and `nb_yfe2_encode_job_run()`.

- [ ] **Step 1: Write failing interleave tests**

```c
for(uint32_t sequence=1;sequence<=8;sequence++)
    CHECK(nb_yfe2_tx_add(&tx,&source[sequence],1000+sequence,&job)==0);
CHECK(nb_yfe2_tx_slot_count(&tx,0)==2);
CHECK(nb_yfe2_tx_slot_count(&tx,1)==2);
CHECK(nb_yfe2_tx_slot_count(&tx,2)==2);
CHECK(nb_yfe2_tx_slot_count(&tx,3)==2);
```

Assert source 1/5 share slot 0, 2/6 slot 1, and block IDs are monotonic.

- [ ] **Step 2: Write failing block-boundary tests**

Assert the 16th shard seals one job, a 9,999us flow idle does not flush, 10,000us idle does flush, a continuously fed block seals at 150ms, actual count is retained, and missing positions are zero-filled through K=16.

- [ ] **Step 3: Write failing adaptive-boundary tests**

Create a baseline block, switch requested R to 3, and assert the existing job remains R=1 while the next block is R=3 with `BURST` set.

- [ ] **Step 4: Implement bounded block state**

```c
typedef struct {
  uint64_t connection_generation;
  uint32_t session_id, block_id;
  uint8_t parity_shards, actual_count;
  uint16_t shard_size;
  nb_yfe2_desc_t desc[16];
  uint8_t source[16][NB_UDP_FRAGMENT_PAYLOAD];
} nb_yfe2_encode_job_t;
```

`nb_yfe2_tx_add()` copies metadata/payload only after the caller has queued the original. Four active slots plus four sealed jobs enforce the per-flow limit of eight.

- [ ] **Step 5: Implement RS encoding and partial semantics**

`nb_yfe2_encode_job_run()` calls `nb_rs_encode(16,R,...)`. Positions `actual_count..15` are known zero shards. Output contains exactly R v3 parity packets; parity failure returns an encode error without modifying the job's source data.

- [ ] **Step 6: Run and commit**

Run: `ctest --test-dir build -R '^nb_yfe2_block$' --output-on-failure`

Commit: `feat: add interleaved NB YFE2 blocks`.

---

### Task 5: Bounded Asynchronous Parity Worker

**Files:**
- Create: `src/nb_yfe2_worker.h`
- Create: `src/nb_yfe2_worker.c`
- Create: `tools/test_yfe2_worker.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: immutable jobs from Task 4.
- Produces: process-owned pthread worker, eventfd notification, bounded submission/result queues, and worker metrics.

- [ ] **Step 1: Write failing queue-cap tests**

Create a worker with test caps `jobs=2`, `bytes=4096`; submit two jobs, block the encoder with a test barrier, and assert a third submission returns `NB_YFE2_WORKER_FULL` without changing accepted-job contents.

- [ ] **Step 2: Write failing generation and shutdown tests**

Assert results carry the submitted connection generation; stale results can be identified without dereferencing a flow pointer. Close while work is queued and assert pthread joins, eventfd closes, and all allocated job/result bytes return to zero.

- [ ] **Step 3: Verify RED**

Run: `cmake --build build --target nb_yfe2_worker_test`

Expected: missing worker API.

- [ ] **Step 4: Implement one process worker and two bounded queues**

```c
#define NB_YFE2_WORKER_MAX_JOBS 128u
#define NB_YFE2_WORKER_MAX_BYTES (16u*1024u*1024u)
typedef struct { uint64_t submitted,completed,dropped,encode_ns;
    size_t job_high,result_high,memory_high; } nb_yfe2_worker_metrics_t;
typedef struct {
    uint64_t connection_generation;
    uint32_t session_id,block_id;
    uint8_t parity_count;
    struct { size_t length; uint8_t wire[NB_YFE2_MAX_PARITY_WIRE]; } parity[3];
} nb_yfe2_encode_result_t;
```

The worker mutex protects queue ownership only. It never calls picoquic, logging, flow lookup, or mutable node state. Successful result insertion writes `uint64_t 1` to eventfd.

- [ ] **Step 5: Run concurrency checks**

Run: `ctest --test-dir build -R '^nb_yfe2_worker$' --output-on-failure`

Run on Linux build host:

```bash
cmake -S . -B test-build-tsan -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build test-build-tsan --target nb_yfe2_worker_test -j2
TSAN_OPTIONS=halt_on_error=1 ./test-build-tsan/nb_yfe2_worker_test
```

Expected: PASS with no TSAN report when compiler supports TSAN; otherwise record `tsan=unavailable` and run the test 1,000 times under normal build.

- [ ] **Step 6: Commit**

Commit: `feat: encode NB YFE2 parity asynchronously`.

---

### Task 6: Bounded Decoder, Recovery Deadline, and Duplicate Suppression

**Files:**
- Create: `src/nb_yfe2_rx.h`
- Create: `src/nb_yfe2_rx.c`
- Create: `tools/test_yfe2_rx.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: standard systematic descriptors, v3 parity view, and `nb_rs_recover()`.
- Produces: per-flow recovery cache with callbacks that return reconstructed `NBUD` bytes exactly once.

- [ ] **Step 1: Write failing recovery fixtures**

Generate literal blocks and assert:

- baseline recovers one missing source with one parity;
- burst recovers three missing sources with three independent parity shards;
- four missing sources return `NB_YFE2_UNRECOVERABLE`;
- recovered source followed by late systematic is delivered once;
- duplicate parity increments duplicate count and returns no payload.

- [ ] **Step 2: Write failing expiry/resource tests**

At 149,999us retain an incomplete block; at 150,000us mark it unrecoverable and free it. Configure test caps `flow_blocks=2`, `flow_bytes=4096`, insert three incomplete blocks, and assert only the oldest incomplete block is evicted. A later unseen systematic from the evicted block must still be delivered directly.

- [ ] **Step 3: Verify RED**

Run: `cmake --build build --target nb_yfe2_rx_test`

Expected: missing receiver API.

- [ ] **Step 4: Implement receiver bounds and reconstruction**

```c
#define NB_YFE2_RX_FLOW_BLOCKS 32u
#define NB_YFE2_RX_FLOW_BYTES (2u*1024u*1024u)
#define NB_YFE2_RX_GLOBAL_BLOCKS 256u
#define NB_YFE2_RX_GLOBAL_BYTES (32u*1024u*1024u)
```

The key is `(connection_generation,session_id,direction,profile_id,block_id)`. Partial blocks treat unused K positions as present zero shards. Before returning a recovered source, encode it with the unchanged `nb_udp_wire_encode()` and validate it with `nb_udp_wire_decode()`.

- [ ] **Step 5: Measure bounded decode cost**

Run 10,000 worst-case `16+3` recoveries in the unit test and emit P50/P99 microseconds. Fail only if a single recovery exceeds the hard watchdog of 20ms; production acceptance later enforces P99 <=2ms.

- [ ] **Step 6: Run and commit**

Run: `ctest --test-dir build -R '^nb_yfe2_rx$' --output-on-failure`

Commit: `feat: recover bounded NB YFE2 blocks`.

---

### Task 7: Effective Physical-Loss Adaptive State

**Files:**
- Create: `src/nb_yfe2_adaptive.h`
- Create: `src/nb_yfe2_adaptive.c`
- Create: `tools/test_yfe2_adaptive.c`
- Modify: `third_party/picoquic/src/picoquic/picoquic.h`
- Modify: `third_party/picoquic/src/picoquic/quicctx.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: cumulative `nb_yfe2_loss_sample_t`, invalid-reason bitmask, and `BASELINE/BURST` state.
- Produces: read-only picoquic adapter `picoquic_get_nb_limit_state()` for app-limited and send-queue-full status.

- [ ] **Step 1: Write failing state tests**

```c
CHECK(nb_yfe2_adaptive_update(&state,&baseline,1000000)==NB_YFE2_BASELINE);
sample.sent+=100; sample.declared_lost+=1;
CHECK(nb_yfe2_adaptive_update(&state,&sample,1200000)==NB_YFE2_BURST);
CHECK(state.burst_until_us==6200000);
CHECK(nb_yfe2_adaptive_update(&state,&sample,6199999)==NB_YFE2_BURST);
CHECK(nb_yfe2_adaptive_update(&state,&sample,6200000)==NB_YFE2_BASELINE);
```

- [ ] **Step 2: Add one test per invalid reason**

Use separate cumulative fixtures for spurious loss, local RX overflow, local TX error, queue pressure drop, scheduler expiry, send queue full, PMTU fallback, app-limited, rate-cap-limited, and probe. Each must leave mode and `burst_until_us` unchanged while incrementing its exact ignored counter.

- [ ] **Step 3: Add regression/reset tests**

Lower each cumulative counter and change connection generation; assert both cases rebuild the baseline and do not trigger burst.

- [ ] **Step 4: Verify RED and implement the pure state machine**

```c
typedef struct {
  uint64_t generation,sent,declared_lost,spurious_lost;
  uint64_t local_drops,scheduler_expired,send_queue_full,pmtu_fallbacks;
  int app_limited,rate_cap_limited,probe;
} nb_yfe2_loss_sample_t;
```

Effective loss is `max(declared_delta-spurious_delta,0)` only for a valid window. One effective loss enters/refreshes BURST for 5,000,000us. No percentage threshold or jitter signal is used.

- [ ] **Step 5: Add the minimal picoquic read-only adapter**

Return a stable struct containing `app_limited` and cumulative `send_queue_full`. Do not change congestion-control behavior. Add a picoquic unit/embedding assertion showing reads do not mutate BBR state.

- [ ] **Step 6: Run and commit**

Run: `ctest --test-dir build -R '^nb_yfe2_adaptive$' --output-on-failure`

Commit: `feat: adapt NB YFE2 from physical loss only`.

---

### Task 8: Node Integration, Probe Isolation, PMTU Fallback, and Scheduling

**Files:**
- Create: `src/nb_yfe2_node.h`
- Create: `src/nb_yfe2_node.c`
- Modify: `src/nb_pool.h`
- Modify: `src/nb_session.h`
- Modify: `src/nb_runtime.h`
- Modify: `src/nb_node_core.inc`
- Modify: `src/nb_node_pool.inc`
- Modify: `src/nb_node_main.inc`
- Modify: `src/nb_node_transport.inc`
- Modify: `src/nb_node_udp_queue.inc`
- Modify: `src/nb_pmtu.h`
- Modify: `src/nb_pmtu.c`
- Create: `tools/test_yfe2_node_contract.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1-7 modules.
- Produces: pure `nb_yfe2_node_on_original_queued()`/`nb_yfe2_node_drain_result()` boundary, per-pool negotiation/adaptive state, per-flow interleaved encoder/receiver, `NB_PRIO_FEC=12`, and parity-worker eventfd integration.

- [ ] **Step 1: Write a failing node contract harness**

The harness uses fake queue callbacks and asserts the exact sequence:

```text
queue original NBUD
record systematic
seal/submit parity job
drain parity result
queue NBUF v3 at priority 12
```

Inject submission failure after the first action and assert return success for the original, `parity_drop=1`, `flow_teardown=0`.

The pure boundary has no picoquic dependency:

```c
typedef struct {
  int (*record_source)(void* context,const nb_udp_wire_view_t* source,uint64_t now_us);
  int (*submit_job)(void* context,const nb_yfe2_encode_job_t* job);
  int (*queue_parity)(void* context,const uint8_t* wire,size_t length,int priority);
  void* context;
} nb_yfe2_node_ops_t;

int nb_yfe2_node_on_original_queued(nb_yfe2_node_ops_t* ops,
    const nb_udp_wire_view_t* source,uint64_t now_us);
int nb_yfe2_node_drain_result(nb_yfe2_node_ops_t* ops,
    const nb_yfe2_encode_result_t* result,uint64_t current_generation);
```

`nb_yfe2_node_on_original_queued()` is called only after ordinary NBUD enqueue succeeds. All FEC callback errors increment parity diagnostics and return zero to the business caller.

- [ ] **Step 2: Add optional-negotiation integration tests**

Assert Middle emits one exact `T:nb-fec-capability.internal:9` proposal after connection ready; Exit intercepts before DNS; a matching response enables only that connection. Timeout, reject, wrong nonce, wrong connection and wrong Profile keep ordinary NBUD active and do not change route health.

- [ ] **Step 3: Add probe-isolation tests**

For `nb-probe-sink.internal`, `nb-probe-source.internal`, and `nb-probe-echo.internal`, assert no block source count, parity, decoder cache, adaptive transition, or hold refresh. The original probe bytes and result hash must match FEC-off fixtures.

- [ ] **Step 4: Add PMTU and direction tests**

Assert v3 enables only when `24 + 16*12 + shard_size <= max_datagram_payload`. One byte below the requirement yields `fallback_pmtu`; original NBUD still queues. S2C, Entry egress, TCP and schema-1 samples never enter v3.

- [ ] **Step 5: Integrate connection lifecycle**

Add `nb_yfe2_link_t` to each `cnx_pool_t` slot, keyed by a monotonically increasing generation. Initialize proposal only for Middle schema-2 egress. On close, invalidate generation before freeing queues so late worker results are dropped safely.

- [ ] **Step 6: Integrate eventfd and timers**

Register the worker eventfd with epoll. Include the earliest 10ms block flush and 500ms negotiation deadline in the existing wait calculation. Drain all ready results without blocking, verify generation/session, and enqueue parity through the ordinary datagram scheduler at priority 12.

- [ ] **Step 7: Replace legacy creation for new v3 sessions**

Keep `NBUF v2` receive handling. For accepted v3 links, stop calling `nb_udp_fec_tx_create_ex()` and use the new block builder. For fallback/schema-1 links, retain current standard or legacy-v2 behavior exactly as configured.

- [ ] **Step 8: Run node and regression tests**

Run: `ctest --test-dir build -R '^(nb_yfe2_node_contract|nb_udp|nb_udp_fec|nb_udp_probe_echo|nb_pmtu|nb_wait|nb_tenant|nb_live)$' --output-on-failure`

Commit only exact integration hunks plus new test with message: `feat: integrate optional NB YFE2 on relay exit`.

---

### Task 9: Structured Diagnostics and Lifetime Accounting

**Files:**
- Create: `src/nb_yfe2_metrics.h`
- Create: `src/nb_yfe2_metrics.c`
- Create: `tools/test_yfe2_metrics.c`
- Modify: `src/nb_metrics.h`
- Modify: `src/nb_metrics.c`
- Modify: `src/nb_node_core.inc`
- Modify: `controlplane/internal/worker/client.go`
- Modify: `controlplane/internal/worker/worker_test.go`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: negotiation, worker, adaptive, tx and rx snapshots.
- Produces: backward-compatible `metrics.fec.nb_yfe2` aggregate and bounded `GET /fec` connection list.

- [ ] **Step 1: Write failing JSON contract tests**

Seed literal counters and assert:

```json
{
  "negotiation":{"state":"accepted","codec":"nb-yfe2","wire_version":3,"profile_id":28909},
  "mode":{"current":"baseline","transitions":2,"effective_physical_loss":3},
  "traffic":{"business_bytes":16000000,"wire_bytes":17000000,"overhead_ratio":0.0625},
  "shards":{"original":16000,"baseline_parity":1000,"burst_parity":30,"recovered":4,"unrecoverable":1,"duplicate":2},
  "resources":{"memory_high_bytes":1048576,"encoder_queue_drop":0,"decoder_queue_drop":1}
}
```

Also assert every 64-bit counter is rendered without truncation and zero business bytes produces overhead ratio 0, not NaN/Inf.

- [ ] **Step 2: Add bounded connection-list tests**

Insert more than the pool connection capacity and assert `GET /fec` returns only live pool slots, never scans flows/logs, and merges closed connection counters into lifetime aggregate exactly once.

- [ ] **Step 3: Implement metrics snapshots**

Use snapshot copies under short locks; format JSON after releasing worker/connection locks. Add ignored-sample counters per reason, encode/decode nanoseconds, queue/memory high water, PMTU fallback and probe suppression.

- [ ] **Step 4: Update worker parser as optional data**

Old Core responses without `nb_yfe2` must decode as unavailable, not zero/healthy. Worker health and line success calculations must not read negotiation state.

- [ ] **Step 5: Run and commit**

Run: `ctest --test-dir build -R '^(nb_yfe2_metrics|nb_metrics)$' --output-on-failure`

Run: `go test ./controlplane/internal/worker`

Commit: `feat: expose NB YFE2 diagnostics`.

---

### Task 10: Canary Tooling, Netem Matrix, Build, and Production Gate

**Files:**
- Create: `tools/nb_yfe2_canary.py`
- Create: `tools/test_yfe2_canary.py`
- Modify: `tools/netem_matrix.py`
- Modify: `tools/test_netem_matrix.py`
- Modify: `tools/fec_canary_run.py`
- Modify: `tools/deploy.py`
- Modify: `tools/line_open.py`
- Modify: `tools/test_line_open.py`
- Modify: `tools/line_provision.py`
- Modify: `tools/test_line_provision.py`

**Interfaces:**
- Consumes: schema-2 renderer, `GET /fec`, existing transactional shard deploy and selective netem.
- Produces: paired worker-index canary with automatic schema-1 rollback and machine-readable evidence.

- [ ] **Step 1: Write failing profile-generation isolation tests**

Assert new line artifacts render Middle egress schema 2 optional and Entry remains schema 1/off. FEC acceptance/readiness must not appear in line-operation success predicates or required request fields.

- [ ] **Step 2: Write failing canary transaction tests**

Using fake nodes, assert order:

```text
verify Exit v3 acceptance capability
publish Exit decoder-only binary
restart selected Exit shard
publish Middle binary with schema 1
restart selected Middle shard
publish schema 2 to matching Middle worker
verify accepted on matching Exit connection
```

Inject failure at each step and assert Middle profile returns schema 1 before any binary rollback. Reject mismatched worker indices.

- [ ] **Step 3: Extend selective netem scenarios**

Add named fixtures for random loss `0,0.1,0.2,0.5,1%`, burst loss `1,3,6,12` packets, reorder, policer, app-limited and rate-cap-limited. Every case records FEC mode, R, recovered/unrecoverable, original drops, probe suppression, throughput and CPU/memory.

- [ ] **Step 4: Add quantitative acceptance predicates**

```python
assert result["open_line_status"] == "unchanged"
assert result["original_queue_drop"] == 0
assert result["probe_parity"] == 0
assert result["fec_off_business_mbps"] * 0.98 <= result["fec_on_business_mbps"]
assert 0.06 <= result["baseline_overhead_ratio"] <= 0.085
assert result["decode_p99_ms"] <= 2.0
assert result["event_loop_p99_delta_ms"] <= 1.0
```

- [ ] **Step 5: Run all local and control-plane tests**

Run: `ctest --test-dir build --output-on-failure`

Run: `go test ./controlplane/...`

Run: `python tools/test_yfe2_canary.py && python tools/test_netem_matrix.py && python tools/test_line_open.py && python tools/test_line_provision.py`

Expected: all pass; no FEC state participates in an open-line hard gate.

- [ ] **Step 6: Build on the configured Linux build host**

Run through the current line inventory: `NB_FORCE_REMOTE_BUILD=1 python tools/deploy.py build`

Expected: full Linux CTest passes, `nb_node` links with pthread/OpenSSL, runtri standard TCP/UDP probes remain unchanged, and the manifest contains every new source/test input.

- [ ] **Step 7: Deploy decoder-only Exit support**

Record current release and active session count. In a maintenance window, deploy the new binary to Exit with schema 1, verify all Exit controls, actual `/proc/<pid>/exe` SHA, v2 compatibility, and no v3 parity emitted.

- [ ] **Step 8: Deploy Middle support with schema 1**

Deploy the same binary to Middle while keeping schema 1. Verify ordinary NBUD throughput/integrity and that negotiation state remains disabled.

- [ ] **Step 9: Run one paired-shard schema-2 canary**

Enable only matching Middle/Exit worker 0, establish new connections, and run the complete matrix. Keep worker 1 schema 1 as the same-host control. Do not expand if any acceptance predicate fails.

- [ ] **Step 10: Observe no-loss production window**

Run for 30 minutes. Require `accepted`, baseline only, overhead 6%-8.5%, business throughput >=98% of control, original queue drop 0, no sustained encoder backlog, and resource limits respected.

- [ ] **Step 11: Expand or roll back**

If all gates pass, enable worker 1 and repeat smoke tests. On failure, first restore Middle worker 0 to schema 1 and verify no new parity; then restore binaries as needed. FEC cleanup failure is reported but must not hold the line-operation lock.

- [ ] **Step 12: Commit canary tooling**

Commit: `test: add NB YFE2 canary and netem gates`.
