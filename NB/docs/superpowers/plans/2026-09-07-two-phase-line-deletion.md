# Two-Phase Line Deletion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete remote line instances before deleting their control-plane records.

**Architecture:** DELETE persists a pending request and queues the existing `line.disable` operation with a deletion marker. Worker performs idempotent three-role cleanup, verifies configuration/socket absence, refreshes runtime claims, then completion atomically finalizes the control-plane deletion.

**Tech Stack:** Go 1.24, MySQL, SQLite telemetry, Python NB deployment tools, systemd shard runtime.

**Spec:** `docs/superpowers/specs/2026-09-07-two-phase-line-deletion-design.md`

## Global Constraints

- Normal deletion must never remove the database record before remote cleanup confirmation.
- Draft and failed-open lines with specs require cleanup.
- Force deletion is allowed only for unreachable/error conditions and preserves runtime claims.
- Cleanup is instance-scoped and must not stop shared shard workers or other lines.
- User-facing messages are Chinese.

---

### Task 1: Persist Pending Deletion And Queue Cleanup

**Files:**
- Modify: `controlplane/internal/central/line_deletion.go`
- Modify: `controlplane/internal/central/mysql.go`
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/webapp/line_delete.go`
- Test: `controlplane/internal/central/line_deletion_test.go`
- Test: `controlplane/internal/webapp/line_delete_test.go`

- [x] Write failing tests proving a draft with a spec remains until a cleanup operation succeeds.
- [x] Add `line_deletion_requests` migration and transactional request/operation creation.
- [x] Return HTTP 202 with the cleanup operation; keep direct deletion only for empty drafts and eligible force requests.
- [x] Run central and webapp tests.

### Task 2: Verify Node Cleanup And Refresh Claims

**Files:**
- Modify: `tools/deploy_core.py`
- Modify: `controlplane/internal/worker/client.go`
- Modify: `controlplane/internal/worker/runtime_ports.go`
- Test: `tools/test_deploy_transaction.py`
- Test: `controlplane/internal/worker/worker_test.go`

- [x] Write failing tests for configuration/socket absence and scan-before-completion ordering.
- [x] Make instance stop wait for both worker configs and control sockets to disappear.
- [x] After deletion-marked `line.disable`, scan all three roles, reject residual target claims, and upload complete batches before success completion.
- [x] Run Python and Worker tests.

### Task 3: Finalize And Deploy

**Files:**
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/webapp/assets/app.js`

- [x] Finalize deletion during successful cleanup completion and preserve normal disable behavior.
- [x] Update UI text to report queued cleanup instead of immediate deletion.
- [x] Run Go, vet, and JavaScript tests.
- [x] Build and transactionally deploy `nb-web` and `nb-web-worker`.
- [x] Precisely remove current orphan configs except `gz-hk-kz-00012_1`, reload shards, refresh claims, and verify released ports.
