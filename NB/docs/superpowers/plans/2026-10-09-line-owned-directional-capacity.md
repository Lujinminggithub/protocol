# Line-Owned Directional Capacity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make each line's single upstream/downstream pair the only business capacity input while retiring physical-link configuration and preserving traffic analysis unchanged.

**Architecture:** Keep `network_links` and historical reservations as compatibility data, but remove them from all current write, governance, admission, and deletion decisions. Continue carrying `upstream_mbps` and `downstream_mbps` through the existing line spec, worker plan, probe, and Node policy paths. Remove the physical-link product surface while retaining traffic history and Entry/Middle/Exit charts.

**Tech Stack:** Go control plane, SQLite/MySQL migrations, vanilla HTML/CSS/JavaScript, Python line orchestration.

**Spec:** `docs/superpowers/specs/2026-10-09-line-owned-directional-capacity-design.md`

## Global Constraints

- Preserve the NB wire format.
- A line supplies exactly one upstream/downstream pair.
- Do not aggregate capacity across lines or physical segments.
- Preserve traffic collection, traffic APIs, Entry/Middle/Exit charts, and evidence timeline unchanged.
- Preserve historical physical-link tables as compatibility data.
- `E:\code\Newbility` is authoritative and final fixes must be byte-synchronized to `E:\project\protocol\NB`.
- Commit in Chinese and do not push.

---

### Task 1: Remove Physical-Link Admission and Governance

**Files:**
- Modify: `controlplane/internal/central/capacity.go`
- Modify: `controlplane/internal/central/governance.go`
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/line_deletion.go`
- Modify: relevant central tests

**Interfaces:**
- Line admission consumes only `LineSpec.UpstreamMbps` and `LineSpec.DownstreamMbps`.
- Historical network-link reads remain available; no new reservations are created.

- [ ] Write failing tests proving production open succeeds without physical links and creates zero reservations.
- [ ] Write failing tests proving capacity/billing governance findings are absent.
- [ ] Implement no-op reservation activation/release behavior for new flows and remove capacity gates.
- [ ] Run all central tests.

### Task 2: Retire Physical-Link Product API and Navigation

**Files:**
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/inventory.go`
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/styles.css`
- Modify: Web tests

**Interfaces:**
- `GET /api/v1/network-links` remains compatible.
- Physical-link mutation returns HTTP 410 with a line-owned capacity message.
- The line form is the only visible capacity input.

- [ ] Write failing asset/API tests for removed navigation, retired mutation, and one upstream/downstream pair.
- [ ] Remove the physical-link view and client-side write workflow.
- [ ] Retire the write handler without deleting historical reads.
- [ ] Run Web tests and desktop/mobile UI smoke tests.

### Task 3: Preserve Directional Open/Validation and Traffic Analysis

**Files:**
- Modify: `controlplane/internal/webapp/app_test.go`
- Modify: `controlplane/internal/worker/worker_test.go`
- Modify: `tools/test_line_open.py`
- Modify only if required: line plan/orchestration code

**Interfaces:**
- `upstream_mbps` and `downstream_mbps` pass unchanged to open, probe, qualification, and Node policy.
- Traffic API/assets remain byte-compatible in behavior.

- [ ] Add regression tests for asymmetric and 10/10 line plans.
- [ ] Add explicit traffic-analysis preservation assertions for total/Entry/Middle/Exit charts and timeline.
- [ ] Run Go, Python, race, and native C tests.

### Task 4: Synchronize, Commit, and Deploy

**Files:**
- Modify: `controlplane/README.md`
- Synchronize all changed files to `E:\project\protocol\NB`.

- [ ] Update operations documentation.
- [ ] Run `git diff --check`, full tests, and SHA comparison.
- [ ] Commit Newbility and NB in Chinese without pushing.
- [ ] Build Web/Worker/Upgrader on `152.32.171.216`, deploy control-plane binaries, and verify health/assets.
- [ ] Confirm data-plane Node binaries and traffic collection were not restarted or changed.
