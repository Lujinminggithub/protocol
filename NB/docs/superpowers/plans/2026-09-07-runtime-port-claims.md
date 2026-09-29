# Runtime Port Claims Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge live shard port claims into allocation and atomically refresh automatic ports immediately before `line.open` dispatch.

**Architecture:** The Worker scans actual shard configuration through existing authenticated SSH paths and replaces claims only after a complete scan. Central allocation and dispatch use both persisted specs and claims; dispatch rewrites automatic ports and the embedded operation plan in one transaction.

**Tech Stack:** Go 1.24, MySQL control database, SQLite tests, `golang.org/x/crypto/ssh`, existing NB Web/Worker Agent API.

**Spec:** `docs/superpowers/specs/2026-09-07-runtime-port-claims-design.md`

## Global Constraints

- Central Web must not initiate SSH connections.
- Failed scans must preserve previous claims.
- Expired claims remain conservative reservations until a complete scan confirms absence.
- Explicit ports must never be silently changed.
- All user-visible validation and conflict messages are Chinese.

---

### Task 1: Persist Claims And Allocation Intent

**Files:**
- Modify: `controlplane/internal/central/mysql.go`
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/inventory.go`
- Test: `controlplane/internal/central/store_test.go`

**Interfaces:**
- Produces: `RuntimePortClaim`, `ReplaceRuntimePortClaims`, claim-aware `AllocateLineSpec` and `LineSpecConflict`.

- [x] Write failing tests for claim persistence, confirmed-absence replacement, stale-claim retention, automatic allocation, and explicit conflict.
- [x] Run `go test ./internal/central -run RuntimePort -count=1` and confirm failure.
- [x] Add migrations, model fields, and transactional claim replacement.
- [x] Merge claims into allocation/conflict queries and preserve automatic intent fields.
- [x] Run central tests.

### Task 2: Agent APIs And Dispatch CAS

**Files:**
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/inventory.go`
- Modify: `controlplane/internal/central/store.go`
- Test: `controlplane/internal/webapp/app_test.go`
- Test: `controlplane/internal/central/store_test.go`

**Interfaces:**
- Consumes: `RuntimePortClaim` and automatic intent fields.
- Produces: `GET /agent/v1/runtime-port-scan-plans`, `POST /agent/v1/runtime-port-claims`, claim-aware operation dispatch.

- [x] Write failing API and dispatch tests using literal claim fixtures.
- [x] Verify failures before implementation.
- [x] Add authenticated Agent endpoints with strict validation and bounded payloads.
- [x] Refresh automatic ports and embedded plan atomically during the Worker preflight after claim; reject explicit conflicts in Chinese.
- [x] Run central and webapp tests.

### Task 3: Worker Runtime Scanner

**Files:**
- Create: `controlplane/internal/worker/runtime_ports.go`
- Modify: `controlplane/internal/worker/client.go`
- Modify: `controlplane/internal/worker/dynamic.go`
- Test: `controlplane/internal/worker/worker_test.go`

**Interfaces:**
- Consumes: Agent scan plans and existing `dialDynamicSSH`/secret resolution.
- Produces: deduplicated complete-scan claim batches for Entry SOCKS/UDP, Relay lanes, and Exit lanes.

- [x] Write failing parser tests with realistic shard configuration fixtures.
- [x] Write failing client tests proving scan-before-preflight ordering and failed-scan preservation.
- [x] Implement strict shard config parsing and remote scan with existing host-key verification.
- [x] Post only complete device-role scans; refresh on startup, heartbeat, and before operation execution.
- [x] Run Worker tests.

### Task 4: End-To-End Verification And Deployment

**Files:**
- Modify: `controlplane/internal/webapp/assets/app.js` only if automatic-intent fields require request changes.
- Test: `controlplane/internal/webapp/js_test/line-actions.test.mjs` only if UI behavior changes.

**Interfaces:**
- Consumes: completed claim/report/dispatch flow.
- Produces: deployed `nb-web` and `nb-web-worker` with rollback backups.

- [x] Run `go test ./cmd/... ./internal/...`, JavaScript tests, and `go vet ./cmd/... ./internal/...`.
- [x] Build Linux amd64 `nb-web` and `nb-web-worker` with `CGO_ENABLED=0`.
- [x] Transactionally deploy both binaries on HK2 and verify readiness/worker heartbeat.
- [x] Confirm the live `1083` claim is present and refreshed by Worker heartbeats.
- [x] Confirm legacy specs remain explicit until the operator re-saves the port as automatic; dispatch preflight tests prove automatic specs update the stored plan and client input consistently.
