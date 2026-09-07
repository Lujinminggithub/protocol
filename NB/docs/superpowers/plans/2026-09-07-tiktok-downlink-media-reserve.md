# TikTok Downlink Media Reserve Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Protect confirmed TikTok video downlink traffic, report logical delivery correctly, and give media strict access to tenant bandwidth before Bulk.

**Architecture:** Preserve narrowly trusted media rules at the runtime classifier. Account logical bytes only after local socket delivery. Extend the existing tenant token bucket with a short shared media reserve while retaining one total product-rate ceiling.

**Tech Stack:** C11, CMake/CTest, picoquic, POSIX shared memory, Python deployment tools

**Spec:** `docs/superpowers/specs/2026-09-07-tiktok-downlink-media-reserve-design.md`

## Global Constraints

- Do not promote all `tiktokcdn` or `cdn` traffic to Media.
- Keep configured product upload/download limits unchanged.
- Apply the same reserve semantics to local and shared tenant state.
- Count logical bytes only after local delivery.
- Roll back the deployed binary if health, integrity, or concurrent media validation fails.

---

### Task 1: Pin Confirmed TikTok Video Rules

**Files:**
- Modify: `tools/test_live.c`
- Modify: `src/nb_live.c`

**Interfaces:**
- Consumes: `nb_live_flow_observe_rule(...)`
- Produces: `teko*` returns `NB_LIVE_FLOW_KEEP` under downlink dominance

- [x] Add a failing assertion that sustained `teko*` downlink remains Media, while an untrusted media rule still returns `NB_LIVE_FLOW_DEMOTE_DOWNLINK`.
- [x] Run `nb_live_test` and confirm the `teko*` assertion fails.
- [x] Add `teko*` to the narrow latency-preserving rule set.
- [x] Run `nb_live_test` and confirm it passes.

### Task 2: Account Logical Delivery

**Files:**
- Modify: `tools/test_live.c`
- Modify: `src/nb_live.h`
- Modify: `src/nb_live.c`
- Modify: `src/nb_node_session.inc`

**Interfaces:**
- Produces: `nb_live_account_payload(uint64_t*, uint64_t*, int, size_t)`
- Consumes: `ps_note_payload(...)` after `logical_note_delivered(...)`

- [x] Add failing direction tests with literal expected C2S/S2C counters.
- [x] Run `nb_live_test` and confirm the new API is missing.
- [x] Implement the direction helper and invoke normal payload accounting only after local delivery.
- [x] Run `nb_live_test` and node compilation.

### Task 3: Add Tenant Media Reserve

**Files:**
- Modify: `tools/test_tenant.c`
- Modify: `src/nb_tenant.h`
- Modify: `src/nb_tenant.c`
- Modify: `src/nb_tenant_shared.h`
- Modify: `src/nb_tenant_shared.c`
- Modify: `src/nb_node_local.inc`
- Modify: `src/nb_node_transport.inc`

**Interfaces:**
- Produces: `nb_tenant_take_class(..., int media)` and `nb_tenant_retry_after_class_us(...)`
- Consumes: current `proxy_stream_t.flow_class` and `downlink_bulk`

- [x] Add local tests proving Media can consume reserved tokens, Bulk cannot cross the reserve, Bulk uses refill above the reserve, and Bulk regains full burst after media idle.
- [x] Add the equivalent shared-state assertions.
- [x] Run `nb_tenant_test` and confirm the reserve assertions fail.
- [x] Implement the 250ms reserve and 3s activity window, including shared-state version migration.
- [x] Route TCP and UDP token acquisition through the class-aware API.
- [x] Run tenant and node tests.

### Task 4: Build, Deploy, and Validate

**Files:**
- No production source additions beyond Tasks 1-3.

**Interfaces:**
- Consumes: current `gz-hk-kz-00012` transactional deployment state
- Produces: verified release or automatic rollback

- [x] Run the focused CTest targets and full local test suite.
- [x] Build the Linux artifact on the configured build host.
- [x] Record current release and deploy transactionally to Entry, Middle, and Exit.
- [x] Run health and single-flow 5Mbps bidirectional integrity checks.
- [x] Run concurrent 5Mbps uplink plus 5Mbps downlink validation and compare three-role counters.
- [ ] Confirm no `teko* flow downlink-demote` event appears; otherwise restore the previous release.
