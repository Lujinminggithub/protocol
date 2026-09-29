# SSH Host Key Enrollment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add explicit SSH host-key enrollment and reject untrusted devices before line deployment.

**Architecture:** The Web service performs a bounded SSH handshake and validates operator-confirmed public keys. The central store persists the trusted device identity, and each dynamic worker operation atomically renders a per-line `known_hosts` file from the confirmed keys in its plan.

**Tech Stack:** Go 1.24, SQLite, `golang.org/x/crypto/ssh`, embedded HTML/JavaScript, existing Go worker and Python deployment tooling.

---

### Task 1: Persist trusted SSH identities

**Files:**
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/inventory.go`
- Test: `controlplane/internal/central/store_test.go`

- [ ] Add a failing migration test that opens an old database, verifies new host-key columns exist, and verifies existing devices become `pending`.
- [ ] Add failing CRUD tests for `SSHHostKey`, `SSHHostKeyType`, `SSHHostKeySHA256`, `SSHHostKeyStatus`, and `SSHHostKeyConfirmedAt`.
- [ ] Run `go test ./internal/central` and confirm failures are caused by missing fields/schema.
- [ ] Add additive `ALTER TABLE` migrations and include the fields in all device scans/upserts.
- [ ] Re-run `go test ./internal/central` and confirm it passes.

### Task 2: Scan and confirm host keys in the Web API

**Files:**
- Create: `controlplane/internal/webapp/host_keys.go`
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/inventory.go`
- Test: `controlplane/internal/webapp/app_test.go`
- Modify: `controlplane/go.mod`
- Modify: `controlplane/go.sum`

- [ ] Add a failing API test using an in-process SSH server. `POST /api/v1/devices/host-key/scan` must return the algorithm, public key, and `SHA256:` fingerprint.
- [ ] Add failing upsert tests that reject missing confirmation and mismatched fingerprints, preserve trust for an unchanged endpoint, and invalidate trust when host or port changes.
- [ ] Run the focused tests and confirm the expected failures.
- [ ] Implement a timeout-bounded SSH handshake with `golang.org/x/crypto/ssh`; capture the presented key in the callback without accepting it for authenticated use.
- [ ] Parse and validate the confirmed key server-side, recompute its SHA256 fingerprint, and persist only matching values.
- [ ] Re-run Web and central tests.

### Task 3: Add explicit confirmation to the device UI

**Files:**
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/app.css`
- Test: `controlplane/internal/webapp/app_test.go`

- [ ] Add a failing asset/API integration assertion proving the form exposes a fingerprint confirmation action and does not directly save a new endpoint.
- [ ] Implement scan-on-submit, render host/port/algorithm/fingerprint, and require a separate confirm click before upsert.
- [ ] Preserve an existing trusted key only when the endpoint is unchanged; otherwise clear the pending confirmation.
- [ ] Re-run Web tests.

### Task 4: Build the worker trust store before deployment

**Files:**
- Modify: `controlplane/internal/worker/dynamic.go`
- Test: `controlplane/internal/worker/worker_test.go`

- [ ] Add a failing test where one dynamic node is pending and assert the exact Chinese prepare error `设备 exit-1 尚未完成 SSH 主机密钥登记`.
- [ ] Add a failing test with port 5222 and assert the generated line file contains `[host]:5222 <algorithm> <key>`.
- [ ] Add a failing update test proving an existing stale per-line file is atomically replaced and no unrelated host remains.
- [ ] Extend `dynamicDevice` with trusted key fields, validate every node before secret resolution, and render the deterministic per-line file through `writePrivateFile`.
- [ ] Remove dynamic reliance on copying the global file while retaining static registry compatibility.
- [ ] Run worker tests and confirm all pass.

### Task 5: Full verification and production rollout

**Files:**
- Verify: `controlplane/...`
- Verify: `tools/deploy_core.py`

- [ ] Run `gofmt` on modified Go files.
- [ ] Run `go test ./...` from `controlplane`.
- [ ] Run `go build ./cmd/nb-web ./cmd/nb-web-worker`.
- [ ] Run repository P0/P1/P2 gates relevant to control-plane and deployment.
- [ ] Confirm production SSH code still uses `RejectPolicy` and no new `AutoAddPolicy` path exists.
- [ ] Deploy the rebuilt Web and worker to HK atomically, let SQLite migrate, and verify service health.
- [ ] Scan `kz-2`, display its SHA256 fingerprint for operator verification, confirm it only after matching the independently supplied fingerprint, and retry the failed line from prepare.
