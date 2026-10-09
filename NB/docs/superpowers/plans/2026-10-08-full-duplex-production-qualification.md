# Production Full-Duplex Qualification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce 90-second concurrent full-duplex qualification at 95% per direction, reserve physical-link capacity independently by direction, and prevent unqualified production lines from becoming active, billable, or client-accessible.

**Architecture:** Add first-class directed network links, transactional line reservations, and durable qualification records to the central store. Production `line.open` becomes a provisioning phase that automatically queues `line.optimize`; only an admitted optimize result atomically activates reservations, the line, and client delivery. Worker locks continue to use actual device roles and add physical-link keys for qualification isolation.

**Tech Stack:** Go 1.26, MySQL/MariaDB, SQLite, Python 3 active probes, embedded HTML/JavaScript, existing NB worker and systemd shard runtime.

**Spec:** `docs/superpowers/specs/2026-10-08-full-duplex-production-qualification-design.md`

## Global Constraints

- Production qualification is one 90-second concurrent upstream/downstream window.
- Each direction must reach 95% of configured rate; 10 Mbps passes at 9.5 Mbps.
- Capacity is reserved per actual device-role link, never by `resource_group`.
- `aggregate-bidirectional` and `unknown` links cannot satisfy production full duplex.
- Production lines cannot use non-production devices.
- Client URLs, QR codes, and billing are unavailable before admitted qualification.
- Rejection is a business result and never enters profile rollout.
- Shared-runtime isolation and the current NB wire format remain unchanged.
- If implementation touches `src/`, bump Node to `V200R001C01 / 2.1.1`; otherwise retain `V200R001C00 / 2.1.0`.
- Synchronize final tracked files from `E:\code\Newbility` to `E:\project\protocol\NB` without disturbing unrelated files.

---

### Task 1: Directed Links and Transactional Reservations

**Files:**
- Create: `controlplane/internal/central/capacity.go`
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/mysql.go`
- Modify: `controlplane/internal/central/inventory.go`
- Test: `controlplane/internal/central/store_test.go`

**Interfaces:**
- Produces `NetworkLink`, `LineCapacityReservation`, `CapacityError`.
- Produces `UpsertNetworkLink`, `NetworkLinks`, `ReserveLineCapacity`, `ActivateLineCapacity`, and `ReleaseLineCapacity` store methods.

- [x] **Step 1: Write failing directional-capacity tests**

Create `gz -> hk` with forward 20 and reverse 10 Mbps. Reserve `line-a` at 10/6, then assert `line-b` at 10/5 fails only reverse capacity with `required=5 available=4`. Retry `line-a` under the same operation and assert no duplicate. Release twice and assert availability returns to 20/10.

- [x] **Step 2: Run RED**

```powershell
go test ./internal/central -run 'NetworkLink|ReserveLineCapacity' -count=1
```

Expected: missing types/methods.

- [x] **Step 3: Add schemas**

Add `network_links` with unique `(from_device_id,from_role,to_device_id,to_role)` and directional capacities. Add `line_capacity_reservations` with unique `(line_id,link_id)`, operation ID, forward/reverse demand, and `reserved|active|released` state in both SQLite and MySQL DDL.

- [x] **Step 4: Implement atomic reservation**

Resolve Entry->Relay and Relay->Exit from the saved spec inside one write transaction. Reject missing, disabled, non-independent, or exhausted links. Sum `reserved` and `active` demand independently for each direction. Make retries and release idempotent.

- [x] **Step 5: Verify and commit**

```powershell
go test ./internal/central -count=1
git add controlplane/internal/central
git commit -m "新增物理链路双向容量预留"
```

### Task 2: Production Topology and Capacity Preflight

**Files:**
- Create: `controlplane/internal/webapp/capacity.go`
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/inventory.go`
- Test: `controlplane/internal/webapp/app_test.go`

**Interfaces:**
- Consumes reservation APIs from Task 1.
- Produces `GET/PUT /api/v1/network-links` and `validateProductionTopology`.

- [x] **Step 1: Write failing API cases**

Assert production open returns 409 before queueing for: test device, missing link, aggregate billing mode, exhausted forward capacity, and exhausted reverse capacity. Assert errors identify link, direction, required, and available Mbps. A test line remains allowed without production reservation.

- [x] **Step 2: Run RED**

```powershell
go test ./internal/webapp -run 'ProductionTopology|NetworkLink|DirectionalCapacity' -count=1
```

- [x] **Step 3: Implement CRUD validation**

Require valid endpoints/roles, positive capacities up to 100000 Mbps, billing mode `independent-directions|aggregate-bidirectional|unknown`, environment `production|test`, and status `ready|maintenance|disabled`.

- [x] **Step 4: Integrate operation preflight**

For production open, verify all device environments and reserve capacity before operation creation. Persist a server-generated reservation snapshot in the request. Compensate reservation if operation creation fails.

- [x] **Step 5: Verify and commit**

```powershell
go test ./internal/central ./internal/webapp -count=1
git add controlplane/internal/central controlplane/internal/webapp
git commit -m "增加生产拓扑和双向容量预检"
```

### Task 3: Durable 95% Qualification

**Files:**
- Create: `controlplane/internal/central/qualification.go`
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/mysql.go`
- Test: `controlplane/internal/central/store_test.go`
- Modify: `tools/line_probe.py`
- Test: `tools/test_line_probe.py`

**Interfaces:**
- Produces `LineQualification`, `SaveLineQualification`, and `LatestLineQualification`.
- Produces evidence fields `duration_seconds=90` and `required_ratio=0.95`.

- [x] **Step 1: Write threshold and persistence tests**

Use literal boundaries: 9.50/10 admits; 9.49/10 rejects only the failing direction. Persist admitted and rejected records and assert latest selection is deterministic by creation time and operation ID.

- [x] **Step 2: Run RED**

```powershell
python tools/test_line_probe.py
go test ./internal/central -run Qualification -count=1
```

Expected: current 0.90 default admits 9.49 and storage is missing.

- [x] **Step 3: Implement explicit probe contract**

Add CLI `--minimum-throughput-ratio` defaulting to `0.95`, validate `0.5..1.0`, include it in cache key/evidence, and retain 90-second concurrent `run_probe_pair`.

- [x] **Step 4: Implement qualification store**

Persist deployment/operation IDs, target/achieved directional Mbps, duration, ratio, status, reasons, and full evidence. Reject malformed JSON and invalid numeric ranges.

- [x] **Step 5: Verify and commit**

```powershell
python tools/test_line_probe.py
go test ./internal/central -count=1
git add tools/line_probe.py tools/test_line_probe.py controlplane/internal/central
git commit -m "持久化百分之九十五全双工资格"
```

### Task 4: Two-Phase Open and Client/Billing Gate

**Files:**
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/line_deletion.go`
- Test: `controlplane/internal/central/line_deletion_test.go`
- Modify: `controlplane/internal/webapp/app.go`
- Test: `controlplane/internal/webapp/app_test.go`
- Test: `controlplane/internal/webapp/line_delete_test.go`
- Modify: `controlplane/internal/worker/runner.go`
- Test: `controlplane/internal/worker/worker_test.go`

**Interfaces:**
- Consumes reservations and qualification records.
- Produces states `provisioning`, `qualification_pending`, `qualification_failed`, and `active`.
- Produces idempotent automatic optimize key `auto-qualify-<line>-<deployment>`.

- [x] **Step 1: Write failing lifecycle tests**

Exercise: open queued -> provisioning; open success -> qualification_pending + optimize queued; admitted optimize -> active + active reservation + client visible; rejected optimize -> qualification_failed + reserved capacity + client hidden; delete -> release. Assert stale-deployment evidence cannot activate a line.

- [x] **Step 2: Run RED**

```powershell
go test ./internal/central ./internal/webapp ./internal/worker -run 'TwoPhase|QualificationPending|ClientGate' -count=1
```

- [x] **Step 3: Change completion semantics**

Production open stores deployment/client material but enters `qualification_pending`. Atomically enqueue one optimize. Optimize completion saves qualification and activates only admitted evidence matching the current deployment.

- [x] **Step 4: Enforce validation rejection**

Keep rejected admission in stage `validation`, include target/achieved/ratio, preserve evidence, and prohibit checkpoint/profile preparation or generation change.

- [x] **Step 5: Gate client material and billing**

Require active line plus admitted qualification matching deployment for client attachment, line-detail client fields, QR, and URL retrieval. Expose only active reservations to billing consumers; keep generated secrets for retry.

- [x] **Step 6: Integrate deletion compensation**

Release capacity after successful normal cleanup. Force delete releases in the deletion-audit transaction. Open failure before activation releases; qualification failure retains reservation until retry or explicit cleanup.

- [x] **Step 7: Verify and commit**

```powershell
go test ./internal/central ./internal/webapp ./internal/worker -count=1
go test -race ./internal/worker -count=1
git add controlplane/internal/central controlplane/internal/webapp controlplane/internal/worker
git commit -m "实现生产开线全双工两阶段门禁"
```

### Task 5: Link Management and Qualification UI

**Files:**
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/styles.css`
- Test: `controlplane/internal/webapp/app_test.go`

**Interfaces:**
- Consumes link/qualification APIs.
- Produces link editor, directional usage, qualification badges, and evidence details.

- [x] **Step 1: Write failing asset/API tests**

Assert distinct forward/reverse inputs, billing mode, environment/status, directional reserved/available values, affected lines, and qualification labels. Assert no single bidirectional-total capacity field is used.

- [x] **Step 2: Run RED**

```powershell
go test ./internal/webapp -run 'NetworkLinkUI|QualificationUI' -count=1
```

- [x] **Step 3: Implement link and lifecycle UI**

Use an operational table with endpoint selects, billing-mode select, two numeric capacity inputs, directional usage bars, and edit action. Show `待验证`, `全双工合格`, `全双工不合格`, and `资源不足`. Details show targets, achieved values, 95% threshold, duration, reasons, deployment, and operation.

- [x] **Step 4: Verify responsive behavior and commit**

```powershell
node --check controlplane/internal/webapp/assets/app.js
go test ./internal/webapp -count=1
git add controlplane/internal/webapp
git commit -m "增加物理链路和全双工资格界面"
```

Use Playwright at desktop/mobile widths and verify long IDs, tables, and dialogs do not overlap.

### Task 6: Historical Production Governance

**Files:**
- Create: `controlplane/internal/central/governance.go`
- Test: `controlplane/internal/central/store_test.go`
- Modify: `controlplane/internal/webapp/app.go`
- Test: `controlplane/internal/webapp/app_test.go`
- Modify: `controlplane/internal/webapp/assets/app.js`

**Interfaces:**
- Produces `AuditProductionLines` and `GET /api/v1/governance/production-lines`.
- Produces codes `maintenance_required`, `capacity_unknown`, `full_duplex_unqualified`, `qualification_required`.

- [x] **Step 1: Write failing governance tests**

Seed production lines with test devices, missing link, aggregate link, stale evidence, and admitted evidence. Assert exact codes and exclusion of test lines.

- [x] **Step 2: Run RED and implement**

```powershell
go test ./internal/central ./internal/webapp -run Governance -count=1
```

Implement a read-only audit. Do not auto-stop traffic. Block client reissue, upgrade, and package expansion while permitting validation, cleanup, deletion, and remediation.

- [x] **Step 3: Surface remediation and commit**

Show finding code, device/link, and required action. Do not provide an override that relabels aggregate capacity.

```powershell
go test ./internal/central ./internal/webapp -count=1
git add controlplane/internal/central controlplane/internal/webapp
git commit -m "增加历史生产线路全双工治理"
```

### Task 7: Full Verification, Rollout, Spain Governance, and Sync

**Files:**
- Modify: `docs/superpowers/plans/2026-10-08-full-duplex-production-qualification.md` (checkboxes only)
- Runtime: `152.32.171.216:/opt/nb-controlplane`
- Source: `E:\code\Newbility`
- Destination: `E:\project\protocol\NB`

**Interfaces:**
- Consumes Tasks 1-6.
- Produces deployed gates, audited Spain state, and synchronized commits.

- [x] **Step 1: Run complete local verification**

```powershell
Push-Location controlplane
go test ./... -count=1
go test -race ./internal/worker -count=1
Pop-Location
python tools/test_line_probe.py
python -m compileall -q tools
node --check controlplane/internal/webapp/assets/app.js
git diff --check
```

- [x] **Step 2: Build and deploy transactionally**

Build/hash Linux Web and worker, embed current worker commit, inspect `go version -m`, and back up remote binaries/source/registry. Deploy, restart, and verify migrations, services, health, heartbeat, and logs; roll back all components on failure. Do not build Node unless `src/` changed.

- [x] **Step 3: Register only authoritative capacities**

Enter provider-confirmed links. Do not mark `gz-55 <-> HK-151` independent; record its aggregate behavior or leave unknown until the provider supplies 10 Mbps per direction.

- [x] **Step 4: Audit Spain without changing generation**

Verify `gz-hk-sp-00001` is flagged for test-device use and unknown/aggregate full-duplex capacity. Preserve generation 5 and current traffic; block new client delivery/upgrades until remediation.

- [x] **Step 5: Prove hard-gate boundaries**

On a controlled candidate, prove 9.49 rejects, 9.50 admits, client material stays hidden before admission, active reservation is directional, and deletion releases both directions.

- [x] **Step 6: Synchronize and test destination**

Copy only changed files, compare SHA256, run Go source-package tests, worker race, Python probe tests, and JavaScript syntax. Preserve unrelated files.

- [x] **Step 7: Commit and audit**

```powershell
git add <changed-files>
git commit -m "同步生产线路全双工资格门禁"
```

Confirm both repositories have no uncommitted tracked changes, services are active, worker version is current, no unqualified line received a new client config, and no push is manually initiated.
