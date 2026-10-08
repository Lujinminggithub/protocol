# NB Hong Kong Single-Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a production-safe `single_hk` topology that runs Entry and Exit on one Hong Kong device, exposes the existing authenticated SOCKS5 client interface, and preserves legacy tri-hop behavior.

**Architecture:** `single_hk` stores one physical device twice as logical Entry and Exit roles. Entry sends TCP and UDP over loopback QUIC directly to the colocated Exit; no Middle service, Middle port, public relay hop, or NB FEC is created. `service_profile=general` is the default; `tiktok_live` only changes policy and validation thresholds.

**Tech Stack:** C11/picoquic runtime, Python deployment tools, Go control plane, SQLite/MariaDB, vanilla JavaScript UI.

**Spec:** `docs/superpowers/specs/2026-10-04-hk-single-node-design.md`

## Global Constraints

- Missing `topology_mode` means `trihop`.
- `single_hk` accepts exactly one physical device represented by logical `entry` and `exit` nodes; it rejects `relay`.
- `service_profile` is exactly `general` or `tiktok_live`; missing means `general`.
- Single-node deployment starts only Entry and Exit roles on the same host and uses `127.0.0.1:<exit_port>` as the Entry next hop.
- NB FEC is disabled for every `single_hk` instance.
- Client URL remains `socks5://user:password@hong-kong-host:socks-port#line-id`.
- Existing tri-hop plans, schemas, deployments, cleanup and client URLs remain compatible.
- All new user-visible errors are Chinese.

---

### Task 1: Persist topology and service profile

**Files:**
- Modify: `controlplane/internal/central/inventory.go`
- Modify: `controlplane/internal/central/store.go`
- Modify: `controlplane/internal/central/mysql.go`
- Test: `controlplane/internal/central/store_test.go`

**Interfaces:**
- Produces: `LineSpec.TopologyMode string`, `LineSpec.ServiceProfile string`, and `NormalizeTopology()`.
- Defaults: `trihop`, `general`.

- [ ] Write central-store tests proving legacy rows default to `trihop/general`, explicit values round-trip, and invalid values are rejected.
- [ ] Run `go test ./internal/central` and verify the new tests fail because fields/schema are absent.
- [ ] Add both columns to SQLite and MariaDB migrations, CRUD SQL, scanning, and normalization.
- [ ] Run `go test ./internal/central` and verify it passes.
- [ ] Commit `feat: persist NB topology and service profiles`.

### Task 2: Validate and materialize a single-HK Worker plan

**Files:**
- Modify: `controlplane/internal/worker/config.go`
- Modify: `controlplane/internal/worker/dynamic.go`
- Modify: `controlplane/internal/worker/runner.go`
- Test: `controlplane/internal/worker/worker_test.go`

**Interfaces:**
- Consumes: persisted topology/profile values.
- Produces: Worker `LineSpec.TopologyMode`, `ServiceProfile`, environment `NB_TOPOLOGY_MODE=single_hk`, source inventory containing Entry and Exit on the same SSH host, and `NB_FEC_V15_ACTIVE=off`.

- [ ] Add failing tests for one-device logical Entry/Exit plans, relay rejection, different-device rejection, unknown profile rejection, and legacy tri-hop acceptance.
- [ ] Run `go test ./internal/worker` and verify failures are topology-specific.
- [ ] Extend dynamic plan validation and source generation; for `single_hk`, emit no Middle and set Exit transport host to `127.0.0.1` while retaining the public device host for SSH and client URL.
- [ ] Make Runner omit Middle arguments/steps and pass `--topology-mode single_hk --service-profile <profile>` to `line_open.py`.
- [ ] Run `go test ./internal/worker` and verify it passes.
- [ ] Commit `feat: materialize single-HK worker plans`.

### Task 3: Deploy Entry and Exit transactionally on one host

**Files:**
- Modify: `tools/line_open.py`
- Modify: `tools/deploy_core.py`
- Modify: `tools/deploy.py`
- Modify: `tools/deploy_shard_runtime.py`
- Test: `tools/test_line_open.py`
- Test: `tools/test_deploy_transaction.py`
- Test: `tools/test_shard_deploy.py`

**Interfaces:**
- Consumes: `topology_mode`, `service_profile`, Entry/Exit inventory.
- Produces: `deployment_roles() -> ("entry", "exit")` for single mode and legacy three-role order otherwise.

- [ ] Add failing tests proving single mode requires Entry/Exit on the same host, generates no Middle artifacts, commands Entry to `127.0.0.1:<exit_port>`, disables FEC, and rolls back Exit then Entry only.
- [ ] Run the three Python test files and verify failures.
- [ ] Add CLI arguments and topology-aware normalization, security material, role iteration, release distribution, activation, current-deployment, rollback, stop, whitelist and cleanup logic.
- [ ] Keep Entry route prefix empty so both TCP and UDP send `T:<target>:<port>` directly to the loopback Exit pool.
- [ ] Run the Python tests and verify both single-node and tri-hop cases pass.
- [ ] Commit `feat: deploy colocated Entry and Exit roles`.

### Task 4: Expose single-HK topology in the control plane

**Files:**
- Modify: `controlplane/internal/webapp/inventory.go`
- Modify: `controlplane/internal/webapp/app.go`
- Modify: `controlplane/internal/webapp/assets/index.html`
- Modify: `controlplane/internal/webapp/assets/app.js`
- Test: `controlplane/internal/webapp/app_test.go`

**Interfaces:**
- Consumes: central `LineSpec` topology/profile.
- Produces: API and UI fields `topology_mode`, `service_profile`; single-node topology editor selection.

- [ ] Add failing HTTP tests for saving/loading `single_hk`, one physical HK device, automatic zero Middle port, Chinese validation errors, and operation request propagation.
- [ ] Run `go test ./internal/webapp` and verify failures.
- [ ] Add topology/profile controls; when `single_hk` is selected, require one Hong Kong device and generate logical Entry/Exit nodes from it while hiding Relay selection.
- [ ] Preserve current tri-hop form behavior.
- [ ] Run Go and existing JavaScript tests.
- [ ] Commit `feat: add single-HK topology controls`.

### Task 5: Make allocation, cleanup and observations topology-aware

**Files:**
- Modify: `controlplane/internal/central/inventory.go`
- Modify: `controlplane/internal/central/runtime_ports.go`
- Modify: `controlplane/internal/worker/runtime_ports.go`
- Modify: `controlplane/internal/worker/snapshot.go`
- Modify: `controlplane/internal/worker/profile_rollout.go`
- Modify: `controlplane/internal/central/line_deletion.go`
- Test: `controlplane/internal/central/store_test.go`
- Test: `controlplane/internal/worker/worker_test.go`

**Interfaces:**
- Produces: topology-aware role set used by allocation, runtime scan, snapshots, tune rejection and cleanup.

- [ ] Add failing tests proving no Relay allocation/claim exists, Entry and Exit claims share one device without collision, snapshots do not require Middle, tune refuses FEC/profile rollout for single mode, and cleanup removes both logical roles before releasing claims.
- [ ] Run central and worker tests and verify failures.
- [ ] Implement role-set helpers and use them at every affected boundary.
- [ ] Run central/worker/webapp test suites.
- [ ] Commit `feat: support single-node lifecycle and telemetry`.

### Task 6: End-to-end regression and operator documentation

**Files:**
- Modify: `README.md`
- Modify: `controlplane/README.md`
- Create: `tools/test_single_hk_plan.py`
- Modify: `docs/superpowers/specs/2026-10-04-hk-single-node-design.md`

**Interfaces:**
- Produces: reproducible dry-run plan validation and documented client setup.

- [ ] Add a dry-run test that builds a one-device source inventory, checks generated commands/artifacts, validates the SOCKS URL, and asserts no Middle service/socket/port appears.
- [ ] Run it first and verify it fails until all preceding interfaces exist.
- [ ] Document creation, client SOCKS5 fields, `general` versus `tiktok_live`, disabled FEC, validation and rollback.
- [ ] Run `python tools/test_single_hk_plan.py`, Python deployment tests, `go test ./...` under `controlplane`, and the relevant CMake/CTest suite.
- [ ] Run `git diff --check` and confirm only intended files are staged.
- [ ] Commit `test: verify Hong Kong single-node lifecycle`.

## Production Gate

- Do not deploy automatically as part of implementation.
- Build the Linux artifact and control-plane binaries after all local tests pass.
- Create a new `hk-single-00001` test line on one explicitly selected Hong Kong device.
- Verify authenticated TCP, UDP Associate, DNS, low-volume traffic, 5Mbps traffic, process CPU/memory, control sockets and exact rollback.
- Observe for 30 minutes before recommending production use.
