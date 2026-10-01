# Control-Plane Build and Line Optimize Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build Node releases only on the control plane, distribute them through Entry and Relay in topology order, and replace separate validation/tuning UI actions with one recoverable `line.optimize` operation while preserving all existing workflows.

**Architecture:** The control plane builds in `/opt/nb-controlplane/data/build`, uploads only to Entry, then existing resumable node-to-node transfer carries the immutable release Entry -> Relay -> Exit using private addresses first. A new server-side `line.optimize` operation owns validation, profile generation, transactional rollout, and readback so browser lifetime is irrelevant. Worker concurrency remains bounded and locks actual device-role resources instead of relying on operator-entered resource groups.

**Tech Stack:** Go 1.26 control plane, Python 3 deployment tools, MySQL/MariaDB operation store, Paramiko SSH, systemd, existing NB shard control sockets.

**Spec:** `docs/superpowers/specs/2026-10-02-control-plane-build-line-optimize-design.md`

## Global Constraints

- Preserve the current NB wire format and online client compatibility.
- Preserve existing `line.open`, `line.validate`, `line.tune`, `line.upgrade`, `line.rollback`, `line.disable`, deletion, whitelist, runtime-port, and client-config behavior.
- Do not migrate or rewrite historical `resource_group` values.
- Build work must stay under `/opt/nb-controlplane`, which is writable under the worker systemd sandbox.
- Upload order is Control Plane -> Entry -> Relay -> Exit; no automatic Control Plane -> Relay/Exit or Entry -> Exit bypass.
- Same-line tasks and tasks sharing any actual device-role resource remain serialized; disjoint topologies may execute concurrently up to the configured worker limit.
- Do not push Git commits unless explicitly requested.

---

### Task 1: Restore the Correct Distribution Topology

**Files:**
- Modify: `tools/deploy.py`
- Modify: `tools/deploy_core.py`
- Modify: `tools/test_deploy_transaction.py`
- Test: `tools/test_deploy_transfer.py`

**Interfaces:**
- Consumes: `_stage_entry_release`, `_copy_release_between_nodes`, `_stage_release`.
- Produces: `act_deploy_socks()` with fixed Control Plane -> Entry -> Relay -> Exit staging order.

- [ ] **Step 1: Update the transaction test to require topology-order transfer**

Assert the event sequence is exactly:

```python
assert [item for item in events if item[0] == "copy"] == [
    ("copy", "entry", "middle"),
    ("copy", "middle", "exit"),
]
```

Also assert no `("copy", "entry", "exit")` event occurs.

- [ ] **Step 2: Run the transaction test and verify it fails**

Run: `python tools/test_deploy_transaction.py`

Expected: FAIL because the interrupted implementation currently sends bytes directly from the control plane to Relay/Exit.

- [ ] **Step 3: Correct the staging sequence**

Implement in `act_deploy_socks()`:

```python
previous["entry"] = _stage_entry_release(clients["entry"], manifest, bindata)
_copy_release_between_nodes(clients["entry"], "entry", clients["middle"], "middle", manifest)
previous["middle"] = _stage_release(clients["middle"], "middle", manifest)
_copy_release_between_nodes(clients["middle"], "middle", clients["exit"], "exit", manifest)
previous["exit"] = _stage_release(clients["exit"], "exit", manifest)
```

Remove the interrupted Control Plane -> Relay/Exit implementation and the Entry -> Exit direct attempt. Revert control-plane direct `private_ip` connection changes in `deploy_core.py`; node-to-node `_transfer_candidates()` already supplies private-first/public-fallback semantics.

- [ ] **Step 4: Run deployment transfer tests**

Run:

```powershell
python tools/test_deploy_transaction.py
python tools/test_deploy_transfer.py
```

Expected: both print `RESULT PASS`.

- [ ] **Step 5: Commit**

```powershell
git add tools/deploy.py tools/deploy_core.py tools/test_deploy_transaction.py
git commit -m "恢复按线路拓扑分级分发Node"
```

### Task 2: Make Control-Plane Local Build the Only Build Path

**Files:**
- Modify: `tools/deploy.py`
- Modify: `tools/node_release_upload.py`
- Modify: `controlplane/internal/worker/runner.go`
- Modify: `controlplane/linux/nb-web-worker.service`
- Create: `tools/test_controlplane_local_build.py`
- Create: `tools/test_node_release_upload.py`

**Interfaces:**
- Consumes: `BUILD_FILES`, `NODE_RELEASE_INPUTS`, `nb_release.create_manifest()`.
- Produces: `act_build()` that always builds locally and writes `build/nb_node` plus `build/release-manifest.json`.

- [ ] **Step 1: Write a failing local-build workspace test**

Cover:

```python
assert deploy.controlplane_build_root() == pathlib.Path("/opt/nb-controlplane/data/build")
assert deploy.build_execution_mode() == "control-plane"
```

Use a temporary override environment variable in the test so no real `/opt` path is modified.

- [ ] **Step 2: Run the new test and verify it fails**

Run: `python tools/test_controlplane_local_build.py`

Expected: FAIL because the helpers do not yet exist.

- [ ] **Step 3: Implement the local build root and build executor**

Set the build root from:

```python
NB_CONTROLPLANE_BUILD_DIR=/opt/nb-controlplane/data/build
```

Make local build the default and remove reliance on inventory `build_host`. Keep the release manifest, build-input snapshot, P0 tests, CMake/CTest, runtri gate, executable permission, and source-change-during-build rejection.

- [ ] **Step 4: Keep upload builds on the same path**

In `node_release_upload.py`, set `NB_CONTROLPLANE_BUILD_DIR` and call the same `deploy.py build` path. Preserve uploaded Node sources while overlaying only the trusted deployment harness files required to fix control-plane orchestration defects.

- [ ] **Step 5: Update the systemd sandbox**

Keep all build writes below `/opt/nb-controlplane`; confirm `ReadWritePaths=/opt/nb-controlplane` remains sufficient. Do not add broad writable paths such as `/opt` or `/`.

- [ ] **Step 6: Run build-path tests**

Run:

```powershell
python tools/test_controlplane_local_build.py
python tools/test_release.py
python tools/test_deploy_transaction.py
python tools/test_node_release_upload.py
```

Expected: PASS. `test_node_release_upload.py` tests orchestration overlay and build environment without executing CMake.

- [ ] **Step 7: Commit**

```powershell
git add tools/deploy.py tools/node_release_upload.py tools/test_controlplane_local_build.py tools/test_node_release_upload.py controlplane/internal/worker/runner.go controlplane/linux/nb-web-worker.service
git commit -m "统一在控制面构建Node发布"
```

### Task 3: Add the Composite `line.optimize` Contract

**Files:**
- Modify: `controlplane/internal/worker/config.go`
- Modify: `controlplane/internal/worker/client.go`
- Modify: `controlplane/internal/worker/runner.go`
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/webapp/app.go`
- Test: `controlplane/internal/webapp/app_test.go`
- Test: `controlplane/internal/worker/worker_test.go`
- Test: `controlplane/internal/central/store_test.go`

**Interfaces:**
- Produces: operation kind `line.optimize`.
- Produces: result fields `evidence`, `transport_profile`, `transport_generation`, and `transport_rollout` in one operation.
- Preserves: `line.validate` and `line.tune` request/result contracts.

- [ ] **Step 1: Write failing API and capability tests**

Test that:

```go
// line.optimize is accepted when the worker supports both validate and tune.
// Existing validate/tune remain accepted.
// An executor advertises optimize only when both underlying capabilities exist.
```

- [ ] **Step 2: Run targeted Go tests and verify failure**

Run:

```powershell
go test ./internal/webapp ./internal/worker ./internal/central
```

Expected: FAIL because `line.optimize` is not allowed.

- [ ] **Step 3: Add the operation kind without removing old kinds**

Add `line.optimize` to allowed kinds and operation validation. Derive capability advertisement from simultaneous `line.validate` and `line.tune` support so old registry files do not need immediate migration.

- [ ] **Step 4: Update successful completion semantics**

Treat successful `line.optimize` like `line.tune`: require a profile result, update `lines.profile`, and monotonically update `transport_generations`.

- [ ] **Step 5: Run targeted tests**

Expected: all targeted packages pass.

- [ ] **Step 6: Commit**

```powershell
git add controlplane/internal/worker controlplane/internal/central/store.go controlplane/internal/central/store_test.go controlplane/internal/webapp/app.go controlplane/internal/webapp/app_test.go
git commit -m "增加验证并调优复合任务"
```

### Task 4: Execute Validation and Tuning in One Recoverable Worker Operation

**Files:**
- Modify: `controlplane/internal/worker/runner.go`
- Modify: `controlplane/internal/worker/profile_rollout.go`
- Create: `controlplane/internal/worker/optimize.go`
- Test: `controlplane/internal/worker/worker_test.go`
- Test: `controlplane/internal/worker/profile_rollout_test.go`

**Interfaces:**
- Consumes: `line_probe.py` schema-2 evidence and `transportprofile.Generate()`.
- Produces: `runOptimize(ctx, line, request, operationDir, environment, logFile, sequence) (Result, error)`.

- [ ] **Step 1: Write failing optimize state-machine tests**

Cover cache hit, validation execution, validation rejection, profile generation, prepare failure, commit failure, readback failure, and success.

- [ ] **Step 2: Verify tests fail before implementation**

Run: `go test ./internal/worker -run 'Optimize|ProfileRollout' -count=1`

- [ ] **Step 3: Implement validation evidence acquisition**

Use the existing validation command with `--cache`. Parse the written JSON into `transportprofile.Probe`; do not plan a profile before validation succeeds.

- [ ] **Step 4: Generate and transactionally roll out the profile**

Allocate the next generation, call `transportprofile.Generate`, then reuse `applyPlannedProfile`. Preserve schema-1 wire downgrade for old Node binaries.

- [ ] **Step 5: Persist checkpoints**

Write an operation-local state file after validation, profile generation, prepare, commit, and readback. On retry, verify deployment ID and cache key before reusing a checkpoint.

- [ ] **Step 6: Run worker tests and race tests**

Run:

```powershell
go test ./internal/worker -count=1
go test -race ./internal/worker -count=1
```

Expected: PASS.

- [ ] **Step 7: Commit**

```powershell
git add controlplane/internal/worker
git commit -m "实现验证调优一体化执行"
```

### Task 5: Enforce Bounded Parallelism and Actual Device Isolation

**Files:**
- Modify: `controlplane/internal/worker/client.go`
- Test: `controlplane/internal/worker/worker_test.go`

**Interfaces:**
- Consumes: static/dynamic plan nodes with `device_id` and `role`; retains `resource_group` for executor authorization only.
- Produces: sorted device-role lock set and global build lock key `global:node-build`.

- [ ] **Step 1: Write concurrency tests**

Prove that:

```go
// Disjoint device topologies overlap in execution.
// Operations sharing any device-role do not overlap.
// Same line operations do not overlap.
// Two node.release.build operations do not overlap.
// Snapshot and heartbeat loops continue during long operations.
```

- [ ] **Step 2: Run tests and verify any missing isolation fails**

Run: `go test ./internal/worker -run 'Concurrent|ResourceGroup|BuildLock' -count=1`

- [ ] **Step 3: Normalize lock selection**

Use sorted multi-lock acquisition to avoid deadlocks:

```go
line operations -> device:<device_id>:<role> for each plan node
node.release.build -> global:node-build
fallback without a group -> line:<line_id>
```

The default worker concurrency remains 4 and configurable through `NB_WEB_WORKER_OPERATION_CONCURRENCY`.

- [ ] **Step 4: Run race and full worker tests**

Expected: PASS with no data races.

- [ ] **Step 5: Commit**

```powershell
git add controlplane/internal/worker/client.go controlplane/internal/worker/worker_test.go
git commit -m "按实际设备完善任务并发隔离"
```

### Task 6: Replace the Two Web Actions with One Product Action

**Files:**
- Modify: `controlplane/internal/webapp/assets/app.js`
- Test: `controlplane/internal/webapp/app_test.go`

**Interfaces:**
- Produces: one visible `line.optimize` action labelled `验证并调优`.
- Preserves: operation detail rendering for historical `line.validate` and `line.tune` records.

- [ ] **Step 1: Write a failing asset test**

Assert the active-line action list contains `line.optimize` and does not render separate validate/tune buttons, while `kindText` still contains all three operation names.

- [ ] **Step 2: Run the test and verify failure**

Run: `go test ./internal/webapp -run NodeUploadUI -count=1` plus the new operation UI test.

- [ ] **Step 3: Update UI actions and detail rendering**

Render `验证并调优`, submit `line.optimize`, and show validation evidence plus rollout results in the same detail panel. Keep historical task detail support.

- [ ] **Step 4: Run Web tests**

Run: `go test ./internal/webapp -count=1`

Expected: PASS.

- [ ] **Step 5: Commit**

```powershell
git add controlplane/internal/webapp/assets/app.js controlplane/internal/webapp/app_test.go
git commit -m "合并线路验证与协议调优入口"
```

### Task 7: Full Regression, Repository Synchronization, and Production Deployment

**Files:**
- Modify: only files changed by Tasks 1-6 in `E:\project\protocol\NB`

**Interfaces:**
- Produces: identical tracked source content in both repositories.
- Produces: updated `nb-web` and `nb-web-worker` binaries on `152.32.171.216`.

- [ ] **Step 1: Run Python deployment gates**

```powershell
python tools/test_release.py
python tools/test_deploy_transaction.py
python tools/test_deploy_transfer.py
python tools/test_line_open.py
python tools/test_line_probe.py
python tools/test_line_provision.py
python tools/test_shard_deploy.py
python tools/test_media_reserve_deploy.py
```

- [ ] **Step 2: Run Go full and race tests**

```powershell
cd controlplane
go test ./... -count=1
go test -race ./internal/worker ./internal/webapp ./internal/central -count=1
```

- [ ] **Step 3: Test control-plane local build without changing live Node**

Run the local build gate on the control plane, verify manifest and SHA256, but do not activate the binary on line nodes during this step.

- [ ] **Step 4: Synchronize both repositories**

Copy only the tracked files changed by this implementation from `E:\code\Newbility` to `E:\project\protocol\NB`. Compare SHA256 for each copied file and leave unrelated/untracked build artifacts untouched.

- [ ] **Step 5: Commit both repositories without pushing**

Use Chinese commit messages. Verify both repositories have no uncommitted tracked changes. Do not run `git push`.

- [ ] **Step 6: Deploy Web and Worker transactionally**

Build Linux amd64 binaries, retain timestamped backups, replace atomically, restart services, and verify:

```text
nb-web.service active
nb-web-worker.service active
worker heartbeat accepted
GET /agent/v1/operations succeeds
```

- [ ] **Step 7: Retry the failed same-topology line operation**

Reuse the request from `op-7f6246fa9583977ab81852ed`. Verify the log contains control-plane local build and the exact transfer order Entry -> Relay -> Exit. Confirm the successful reference operation `op-9018a0375f11bc58aeaacdab` remains unaffected.

- [ ] **Step 8: Run one real `line.optimize`**

Verify one operation contains validation evidence, generated profile, prepare/commit/readback events, and a successful generation update. Confirm other active lines retain their deployment IDs and sessions.

- [ ] **Step 9: Final operational checks**

Check disk space, stale source candidates, service logs, active operations, runtime port claims, and client configuration synchronization. Report any pre-existing warnings separately from regressions introduced by this change.
