# Force Delete Unreachable Line Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow an administrator to remove an active or maintenance line after a node becomes unreachable, with explicit confirmation and durable audit evidence.

**Architecture:** Extend the existing line deletion request and store transaction with an explicit force-delete policy. Keep normal deletion unchanged, reject active operations in both modes, and add a dedicated Web confirmation flow that makes residual remote resources visible to the operator.

**Tech Stack:** Go `net/http`, SQLite, embedded HTML/JavaScript, Go tests, Node syntax checks.

---

### Task 1: Store Policy And Audit

**Files:**
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/store_test.go`

- [ ] **Step 1: Write a failing store test**

Add a configured active line and assert normal deletion fails, force deletion succeeds only when there are no active operations, and `line_deletion_audit.snapshot` records `force=true` plus device health.

- [ ] **Step 2: Run the focused test and verify RED**

Run: `go test ./internal/central -run TestForceDeleteLine -count=1`

Expected: FAIL because the store has no force-delete request type or policy.

- [ ] **Step 3: Implement the minimal store policy**

Introduce a `LineDeletionRequest` value containing requester, reason, force, confirmation, and orphan acknowledgement. Validate the force confirmation, keep the active-operation conflict, include latest device health in the audit snapshot, and bypass only the disabled-status check when force is valid.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run: `go test ./internal/central -run TestForceDeleteLine -count=1`

Expected: PASS.

### Task 2: HTTP Contract

**Files:**
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/app_test.go`

- [ ] **Step 1: Write failing API tests**

Cover missing line-ID confirmation, missing orphan acknowledgement, successful force deletion, and conflict while an operation is active.

- [ ] **Step 2: Run the focused tests and verify RED**

Run: `go test ./internal/webapp -run TestForceDeleteLine -count=1`

Expected: FAIL because the handler ignores force-delete fields.

- [ ] **Step 3: Implement request decoding and Chinese errors**

Decode the force-delete fields, pass the structured request into the store, and map validation failures to HTTP 400 and state conflicts to HTTP 409.

- [ ] **Step 4: Run the focused tests and verify GREEN**

Run: `go test ./internal/webapp -run TestForceDeleteLine -count=1`

Expected: PASS.

### Task 3: Operator Confirmation UI

**Files:**
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/styles.css`
- Modify: `controlplane/internal/webapp/js_test/topology-layout.test.mjs`

- [ ] **Step 1: Add a failing frontend behavior test**

Assert the action selector returns normal delete for deletable states and force delete for configured active or maintenance states.

- [ ] **Step 2: Run the frontend test and verify RED**

Run: `node --test internal/webapp/js_test/*.test.mjs`

Expected: FAIL because force-delete action selection is missing.

- [ ] **Step 3: Implement the force-delete dialog**

Render a `强制删除` action, require exact line-ID input, a non-empty reason, and an explicit residual-resource acknowledgement. Submit the force fields only from that dialog.

- [ ] **Step 4: Run frontend tests and syntax checks**

Run: `node --test internal/webapp/js_test/*.test.mjs`

Run: `node --check internal/webapp/assets/app.js`

Expected: PASS.

### Task 4: Regression Verification And Deployment

**Files:**
- Verify all files above.

- [ ] **Step 1: Run related Go tests**

Run: `go test ./internal/central ./internal/webapp -count=1`

Expected: PASS.

- [ ] **Step 2: Run static checks and build**

Run: `go vet ./internal/central ./internal/webapp ./cmd/nb-web`

Run: `go build ./cmd/nb-web`

Expected: PASS.

- [ ] **Step 3: Deploy atomically to HK**

Build the Linux `nb-web`, back up the current binary and database, atomically replace the binary, restart `nb-web.service`, and leave `nb-web-worker.service` running.

- [ ] **Step 4: Read back production state**

Verify `/readyz`, service restart count, binary hash, the target failed disable operation, and that no line is deleted until the operator completes the new confirmation dialog.
