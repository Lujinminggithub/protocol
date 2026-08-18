# Realtime Traffic And Device Topology Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add durable per-hop traffic history, live line charts, and a stable interactive 3D device topology to the NB Go control plane.

**Architecture:** SQLite aggregates snapshots transactionally into 60-second and 300-second buckets while retaining seven days of raw data. Go exposes authenticated history/topology endpoints and a non-blocking SSE hub; the embedded frontend renders synchronized ECharts panels and a local Three.js force graph with a two-dimensional fallback.

**Tech Stack:** Go 1.24, modernc SQLite, vanilla JavaScript, ECharts, Three.js, 3D Force Graph, embedded static assets.

---

### Task 1: Traffic storage and retention

**Files:**
- Create: `controlplane/internal/central/traffic.go`
- Modify: `controlplane/internal/central/store.go`
- Test: `controlplane/internal/central/store_test.go`

- [ ] **Step 1: Write failing tests for transactional 60/300-second aggregation, role sums, automatic resolution and retention.**
- [ ] **Step 2: Run `go test ./internal/central -run 'Traffic|Retention' -count=1` and confirm the new tests fail.**
- [ ] **Step 3: Add `traffic_rollups`, transactional rollup upserts, bounded history queries and hourly retention cleanup.**
- [ ] **Step 4: Re-run the focused tests and confirm they pass.**

### Task 2: Authenticated history and SSE APIs

**Files:**
- Create: `controlplane/internal/webapp/traffic.go`
- Create: `controlplane/internal/webapp/stream.go`
- Modify: `controlplane/internal/webapp/app.go`
- Test: `controlplane/internal/webapp/app_test.go`

- [ ] **Step 1: Write failing HTTP tests for valid/invalid history ranges, missing lines, Bearer auth, SSE delivery and slow-subscriber replacement.**
- [ ] **Step 2: Run `go test ./internal/webapp -run 'Traffic|SnapshotStream' -count=1` and confirm failure.**
- [ ] **Step 3: Implement the handlers and snapshot hub; broadcast only after `RecordSnapshot` reports an inserted row.**
- [ ] **Step 4: Re-run focused tests and confirm they pass.**

### Task 3: Topology API

**Files:**
- Create: `controlplane/internal/central/topology.go`
- Modify: `controlplane/internal/webapp/app.go`
- Test: `controlplane/internal/central/store_test.go`
- Test: `controlplane/internal/webapp/app_test.go`

- [ ] **Step 1: Write failing tests proving shared devices are deduplicated and edges preserve every line ID and next hop.**
- [ ] **Step 2: Run `go test ./internal/central ./internal/webapp -run Topology -count=1` and confirm failure.**
- [ ] **Step 3: Implement `Store.Topology` and `GET /api/v1/topology` using devices, line nodes and latest role snapshots.**
- [ ] **Step 4: Re-run focused tests and confirm they pass.**

### Task 4: Embedded frontend dependencies

**Files:**
- Create: `controlplane/internal/webapp/assets/vendor/echarts.min.js`
- Create: `controlplane/internal/webapp/assets/vendor/three.min.js`
- Create: `controlplane/internal/webapp/assets/vendor/3d-force-graph.min.js`
- Modify: `controlplane/internal/webapp/assets/index.html`

- [ ] **Step 1: Pin dependency versions and download their published browser builds into `assets/vendor`.**
- [ ] **Step 2: Record SHA-256 hashes and verify the files are served with HTTP 200 by the embedded handler.**
- [ ] **Step 3: Add local script tags in dependency order and confirm CSP makes no external request.**

### Task 5: Synchronized line traffic charts

**Files:**
- Create: `controlplane/internal/webapp/assets/traffic-charts.js`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/styles.css`
- Test: `controlplane/internal/webapp/app_test.go`

- [ ] **Step 1: Add a static contract test for the chart containers, range controls and local dependency references.**
- [ ] **Step 2: Render overview plus Entry/Middle/Exit charts, connect their axes/crosshairs, preserve gaps as nulls and add deployment/task markers.**
- [ ] **Step 3: Add authenticated fetch-stream parsing with cancellation, reconnect status and incremental point append.**
- [ ] **Step 4: Run webapp tests and a browser check for 30m/2h/6h/24h/7d/30d ranges.**

### Task 6: Three-dimensional device topology

**Files:**
- Create: `controlplane/internal/webapp/assets/device-topology.js`
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Modify: `controlplane/internal/webapp/assets/styles.css`

- [ ] **Step 1: Add unframed overview and device-page topology stages with filters and accessible fallback containers.**
- [ ] **Step 2: Build procedural monitor/base/status-light node meshes and thin health-aware links from `/api/v1/topology`.**
- [ ] **Step 3: Persist cooled node coordinates by device ID, implement device click and line focus, and link line selection to detail charts.**
- [ ] **Step 4: Add WebGL failure fallback to the existing semantic two-dimensional topology.**

### Task 7: Verification and HK deployment

**Files:**
- Modify only if required by deployment: `controlplane/linux/*`

- [ ] **Step 1: Run `gofmt` on changed Go files and `go test ./... -count=1`.**
- [ ] **Step 2: Build `nb-web` and verify embedded vendor assets and API routes locally.**
- [ ] **Step 3: Use browser screenshots and canvas pixel checks at desktop and mobile viewports; confirm no blank canvas or overlapping controls.**
- [ ] **Step 4: Atomically deploy the new `nb-web` to HK, restart only `nb-web`, then verify `/healthz`, `/readyz`, traffic history, SSE and topology.**
- [ ] **Step 5: Cross-check one live line sample against the worker snapshot and report the deployed version and evidence.**

