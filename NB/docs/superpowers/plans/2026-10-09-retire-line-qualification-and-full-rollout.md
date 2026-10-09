# Retire Line Qualification And Full Rollout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the obsolete full-duplex qualification gate, restore truthful production governance, and upgrade all seven active lines through the Web platform-upgrade workflow.

**Architecture:** Historical qualification data remains compatible, but current governance and client delivery no longer depend on probe evidence that the product no longer generates. Device-environment findings remain real governance signals. A new immutable platform candidate is built from the committed Newbility repository and rolled through two transitive shared-runtime units via the existing Web upgrade transaction.

**Tech Stack:** Go control plane, MySQL/SQLite, vanilla HTML/CSS/JavaScript, Python platform upgrade orchestration, NB C runtime.

**Spec:** `docs/superpowers/specs/2026-10-09-retire-line-qualification-and-full-rollout-design.md`

## Global Constraints

- Preserve NB wire format.
- Preserve historical `line_qualifications` data and read compatibility.
- Keep `maintenance_required` for production lines using non-production devices.
- Preserve traffic collection, total/Entry/Middle/Exit charts, and evidence timeline unchanged.
- `E:\code\Newbility` is authoritative and all final changes must be byte-synchronized to `E:\project\protocol\NB`.
- Commit in Chinese and do not push.
- Use the Web platform candidate and upgrade endpoints for the data-plane rollout.

---

### Task 1: Retire Qualification Governance

**Files:**
- Modify: `controlplane/internal/central/governance.go`
- Modify: `controlplane/internal/central/qualification.go`
- Modify: `controlplane/internal/central/store_test.go`
- Test: `controlplane/internal/central/line_owned_capacity_test.go`

**Interfaces:**
- `AuditProductionLines(context.Context) ([]GovernanceFinding, error)` continues returning device-environment findings.
- `ClientDeliveryAllowed(context.Context, string) error` requires an active line and no current governance findings, but not a qualification record.

- [ ] Add a failing test where an active production line without qualification is deliverable.
- [ ] Add a failing test where the same line with a test-environment device remains blocked.
- [ ] Remove qualification reads and `qualification_required` construction from governance.
- [ ] Remove the final admitted-qualification check from client delivery.
- [ ] Run central tests and confirm both new tests pass.

### Task 2: Remove Qualification Product Surface

**Files:**
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/styles.css`
- Modify: `controlplane/internal/webapp/app_test.go`
- Test: `controlplane/internal/webapp/line_owned_capacity_test.go`

**Interfaces:**
- The lines table has eight columns: line, environment, status, topology, upstream/downstream, port, profile, actions.
- Line detail retains governance and traffic sections but has no qualification section.

- [ ] Add failing asset assertions that qualification labels, filters, and detail markup are absent.
- [ ] Keep assertions proving total/Entry/Middle/Exit traffic and evidence timeline are present.
- [ ] Remove qualification state/rendering and adjust line table column count.
- [ ] Remove qualification-only styles while retaining governance styles.
- [ ] Run Web tests and desktop/mobile UI smoke tests.

### Task 3: Synchronize And Deploy Control Plane

**Files:**
- Modify: `controlplane/README.md`
- Synchronize changed files and this plan/spec to `E:\project\protocol\NB`.

**Interfaces:**
- Production Web exposes the updated line-management assets.
- Worker and Upgrader remain compatible with historical qualifications.

- [ ] Document the retired gate and retained historical data.
- [ ] Run Go full tests, race tests, Python orchestration tests, C build/CTest, and `git diff --check`.
- [ ] Commit Newbility and NB in Chinese without pushing.
- [ ] Build Web/Worker/Upgrader on `152.32.171.216`, replace with rollback protection, and verify service/assets/telemetry.

### Task 4: Repair Device Classification And Build Web Candidate

**Files:**
- No tracked source files; operations are recorded by Web tasks and database audit evidence.

**Interfaces:**
- Web device edit changes only `environment` for `gz-55` and `HK-151` to `production`.
- Web source upload produces a successful `node.release.build` with `platform_release` metadata.

- [ ] Use the Web device workflow to classify `gz-55` and `HK-151` as production without changing connectivity fields.
- [ ] Confirm `maintenance_required` disappears for `gz-hk-sp-00001`.
- [ ] Upload the committed Newbility Git repository through Web and wait for candidate build success.
- [ ] Verify candidate manifest hashes and control-plane build evidence.

### Task 5: Upgrade All Active Lines Through Web

**Files:**
- No tracked source files; results are Web `platform.upgrade` operation evidence.

**Interfaces:**
- Unit one is seeded by `gz-hk-sp-00001` and must include `gz-hk-us-00001`.
- Unit two is seeded by `gz-hk-uk-10002` and must include `gz-hk-us-10001` through `gz-hk-us-10004`.

- [ ] Preview unit one and verify the exact two-line impact set before confirmation.
- [ ] Start unit one through Web, wait for success, and verify both lines and all device roles.
- [ ] Preview unit two and verify the exact five-line impact set before confirmation.
- [ ] Start unit two through Web, wait for success, and verify all five lines and all device roles.
- [ ] Confirm all seven lines remain active, runtime versions match the candidate, traffic telemetry advances, and no upgrade operation remains queued/running/failed.
