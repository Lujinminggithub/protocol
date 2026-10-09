# Platform Upgrade Center Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add one administrator-only upgrade center that builds a unified candidate and transactionally replaces scripts, control-plane binaries, and shared Node workers with a two-second per-process health gate and automatic rollback.

**Architecture:** Extend the existing source-upload release pipeline to produce an immutable platform bundle without activating it. Store and expose platform upgrade operations through the existing operations/event model, while an independent `nb-upgrader` executor runs a dedicated transactional Python orchestrator so Web and Worker can restart safely. Shared Node binaries are staged control-plane to Entry to Middle to Exit, then activated Exit to Middle to Entry one worker at a time without an active-session drain gate.

**Tech Stack:** Go 1.22 control plane, Python 3 deployment tooling, MySQL/SQLite-compatible central store, systemd, vanilla HTML/CSS/JavaScript, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-10-08-platform-upgrade-center-design.md`

## Global Constraints

- Keep the NB wire format unchanged.
- Existing line instance configs, ports, certificates, whitelist, and transport profiles are immutable during upgrade.
- A process must become healthy within two seconds or be rolled back.
- Active Node sessions may be terminated and must be recorded, not used as an upgrade refusal gate.
- Stage Node control-plane -> Entry -> Middle -> Exit; activate Exit -> Middle -> Entry, worker 0 then worker 1.
- Build all production artifacts on the control-plane host with managed toolchains.
- `E:\code\Newbility` is authoritative and every final change is copied byte-for-byte to `E:\project\protocol\NB`.
- Commit in Chinese and do not push.

---

### Task 1: Immutable Unified Platform Candidate

**Files:**
- Modify: `tools/node_release_upload.py`
- Modify: `tools/nb_release.py`
- Create: `tools/platform_release.py`
- Modify: `tools/test_node_release_upload.py`
- Create: `tools/test_platform_release.py`
- Modify: `tools/controlplane_source_sync.py`

**Interfaces:**
- Consumes: uploaded Git archive, current control-plane root, managed Go at `NB_CONTROLPLANE_GO` or `/opt/nb-controlplane/toolchains/go/bin/go`.
- Produces: `platform-release.json` with `release_id`, `source_digest`, `scripts`, `nb_web`, `nb_web_worker`, `nb_node`, `candidate_root`, and rollback metadata.

- [ ] **Step 1: Write failing tests for candidate immutability and the unified manifest**

  Add tests that assert source upload leaves `/opt/nb-controlplane/repo` unchanged, stores a ready candidate, requires a control-plane Go toolchain, and records all three component hashes.

- [ ] **Step 2: Run candidate tests and verify RED**

  Run: `python tools/test_node_release_upload.py && python tools/test_platform_release.py`

  Expected: failure because platform manifests and non-activating candidates do not exist.

- [ ] **Step 3: Implement manifest generation and managed Go builds**

  Build `nb-web` and `nb-web-worker` into the candidate, validate `nb_node`, hash the script snapshot, and write the unified manifest atomically. Do not call the old `activate()` during build.

- [ ] **Step 4: Run candidate tests and release tests**

  Run: `python tools/test_node_release_upload.py && python tools/test_platform_release.py && python tools/test_release.py`

  Expected: PASS.

### Task 2: Platform Upgrade API and Impact Preview

**Files:**
- Create: `controlplane/internal/central/platform_release.go`
- Modify: `controlplane/internal/central/mysql.go`
- Modify: `controlplane/internal/central/store.go`
- Create: `controlplane/internal/central/platform_release_test.go`
- Create: `controlplane/internal/webapp/platform_upgrade.go`
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/app_test.go`

**Interfaces:**
- Produces: `GET /api/v1/platform-releases/status`, `GET /api/v1/platform-upgrades/preview`, `POST /api/v1/platform-upgrades`, and `POST /api/v1/platform-upgrades/{id}/rollback`.
- Creates operations with target `__platform__` and kinds `platform.upgrade` or `platform.rollback`.

- [ ] **Step 1: Write failing store and HTTP tests**

  Assert strict JSON, administrator authentication, idempotency, one active platform upgrade, physical-device impact closure, affected-line preview, and rejection of stale previews.

- [ ] **Step 2: Run focused Go tests and verify RED**

  Run: `go test ./internal/central ./internal/webapp -run 'Platform|Upgrade'`

  Expected: compile or route failures because the APIs do not exist.

- [ ] **Step 3: Implement platform release persistence and handlers**

  Add a cross-database platform release table, preview computation from active line topology, operation creation, and a platform upgrade lock represented by the single active `__platform__` operation.

- [ ] **Step 4: Remove line-scoped shared runtime upgrades**

  Reject new `line.upgrade` operations with a Chinese message directing administrators to the upgrade center. Preserve historical operations and rollback records.

- [ ] **Step 5: Run central and Web tests**

  Run: `go test ./internal/central ./internal/webapp`

  Expected: PASS.

### Task 3: Independent Upgrader and Two-Second Transaction

**Files:**
- Create: `controlplane/cmd/nb-upgrader/main.go`
- Create: `controlplane/internal/upgrader/runner.go`
- Create: `controlplane/internal/upgrader/runner_test.go`
- Create: `controlplane/linux/nb-upgrader.service`
- Modify: `controlplane/linux/install.sh`
- Create: `tools/platform_upgrade.py`
- Create: `tools/test_platform_upgrade.py`
- Modify: `tools/deploy_shard_runtime.py`
- Modify: `tools/test_shard_runtime.py`

**Interfaces:**
- Upgrader heartbeat capability: line `__platform__`, operations `platform.upgrade` and `platform.rollback`.
- Orchestrator CLI: `platform_upgrade.py upgrade --manifest PATH --operation-id ID --preview-digest DIGEST` and `rollback --release-id ID`.
- Result includes component stages, per-worker interrupted sessions, downtime milliseconds, health evidence, and rollback evidence.

- [ ] **Step 1: Write failing Python tests for order, forced restart, two-second gate, and reverse rollback**

  Use fake remote clients to assert staging Entry -> Middle -> Exit, activation Exit 0/1 -> Middle 0/1 -> Entry 0/1, no active-session refusal, config counts preserved, and rollback in exact reverse order.

- [ ] **Step 2: Run Python tests and verify RED**

  Run: `python tools/test_platform_upgrade.py && python tools/test_shard_runtime.py`

  Expected: failures because forced rolling activation is absent.

- [ ] **Step 3: Implement the transactional Python orchestrator**

  Add atomic script and control-plane symlink swaps, SHA/version gates, service restart deadlines, Node staging/distribution, rolling worker restarts, probe cleanup, smoke tests, audit output, and rollback.

- [ ] **Step 4: Write failing Go tests for the independent executor**

  Assert the upgrader advertises only `__platform__`, never claims line work, passes operation event context, persists results, and survives temporary Web restart errors.

- [ ] **Step 5: Implement `nb-upgrader` using the existing worker client protocol**

  Extract or expose the minimal generic client configuration needed for a dedicated registry and runner. Keep `nb-web-worker` capabilities unchanged except removal of `line.upgrade`.

- [ ] **Step 6: Run upgrader and worker race tests**

  Run: `go test -race ./internal/upgrader ./internal/worker`

  Expected: PASS.

### Task 4: Upgrade Center UI

**Files:**
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/styles.css`
- Modify: `controlplane/internal/webapp/app_test.go`

**Interfaces:**
- Uses the platform release status, preview, create-upgrade, rollback, operations, and event APIs.

- [ ] **Step 1: Write failing rendered-asset tests**

  Assert a top-level “版本升级” navigation item, current/candidate version matrices, affected-line preview, explicit active-session interruption confirmation, and no line-level upgrade button.

- [ ] **Step 2: Run Web tests and verify RED**

  Run: `go test ./internal/webapp -run 'Platform|Upgrade|Assets'`

  Expected: missing UI markers.

- [ ] **Step 3: Implement the operational UI**

  Add the upgrade view, upload/build action, unit selector, preview, confirmation modal, task launch, progress timeline links, rollback action, and responsive navigation. Keep existing task upload code as a redirect to the new view.

- [ ] **Step 4: Run Web tests and browser smoke checks**

  Run: `go test ./internal/webapp`

  Then run the existing control-plane server and verify desktop/mobile layouts with Playwright screenshots.

### Task 5: Full Verification, Synchronization, and Controlled Deployment

**Files:**
- Modify: `controlplane/README.md`
- Modify: `tools/p0_gate.py`
- Modify: `tools/deploy.py`
- Modify: `tools/controlplane_source_sync.py`

**Interfaces:**
- Produces synchronized commits and deployable `nb-web`, `nb-web-worker`, `nb-upgrader`, and `nb_node` artifacts.

- [ ] **Step 1: Add upgrader tests and files to release/source gates**

- [ ] **Step 2: Run all Go, Python, and native tests**

  Run: `go test ./...`, `go test -race ./internal/worker ./internal/upgrader`, Python P0 tests, Linux CMake build, and `ctest --output-on-failure`.

- [ ] **Step 3: Verify repository and release integrity**

  Run `git diff --check`, confirm version/manifest hashes, and review the complete patch for secrets and unrelated files.

- [ ] **Step 4: Commit Newbility and synchronize NB**

  Commit in Chinese, copy every changed file to `E:\project\protocol\NB`, compare SHA-256, rerun focused tests there, and commit in Chinese without pushing.

- [ ] **Step 5: Deploy control-plane components before data-plane rollout**

  Build all candidates on `152.32.171.216`, install/enable `nb-upgrader`, deploy Web/Worker, confirm all health endpoints, then launch the platform upgrade through the new API. Verify per-worker downtime evidence and final versions; automatic rollback must remain armed until all smoke tests pass.
