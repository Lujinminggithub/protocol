# KZ Lane-Aware Startup Seed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent KZ live startup bursts from overflowing the Middle queue by generating a valid long-haul QUIC seed and adapting it to each pool lane.

**Architecture:** Go generates a cold-start seed from load-qualified QUIC RTT evidence while preserving the committed average rate. A focused C planner scales that seed from the existing lane RTT during make-before-break, and metrics expose whether picoquic actually applied it.

**Tech Stack:** Go control plane, C11 data plane, picoquic BBR, CMake/CTest, Python deployment tools.

---

### Task 1: Control-Plane Seed Calculation

**Files:**
- Modify: `controlplane/internal/transportprofile/profile_test.go`
- Modify: `controlplane/internal/transportprofile/profile.go`

- [ ] Change the long-haul test to require `SeedRTTUS` from `QUIC.RTTP95MS` and a two-times-average startup BDP computed from that RTT.
- [ ] Run `go test ./internal/transportprofile` and confirm the existing ICMP-based implementation fails the new assertion.
- [ ] Move BBR seed selection after congestion-control selection and use loaded QUIC RTT for BBR while leaving the committed target rate unchanged.
- [ ] Re-run `go test ./internal/transportprofile` and require PASS.

### Task 2: Lane-Aware C Seed Planner

**Files:**
- Create: `src/nb_transport_seed.h`
- Create: `src/nb_transport_seed.c`
- Create: `tools/test_transport_seed.c`
- Modify: `CMakeLists.txt`
- Modify: `tools/deploy.py`

- [ ] Add a test requiring a 202757 us/262144 byte profile seed to become a 281000 us/393216 byte effective seed for a valid lane sample, while zero or invalid samples retain the profile values.
- [ ] Configure and run only `nb_transport_seed_test`; confirm it fails before the module exists.
- [ ] Implement a pure planner that validates BBR inputs, uses lane RTT when valid, scales safely, rounds to 64 KiB, and enforces 256 KiB through 64 MiB bounds.
- [ ] Add the focused test target and production source/build manifest entries, then require the test to pass.

### Task 3: Runtime Integration and Applied-State Metrics

**Files:**
- Modify: `src/nb_instance.h`
- Modify: `src/nb_node_core.inc`
- Modify: `src/nb_node_pool.inc`
- Modify: `src/nb_metrics.h`
- Modify: `src/nb_metrics.c`
- Modify: `tools/test_metrics.c`

- [ ] Extend the metrics test to require a `bdp_seed` JSON object containing configured/applied counts and effective RTT/window maxima; confirm the test fails.
- [ ] Store the effective seed plan in `nb_sched_cnx_t`, pass `pool->recent_rtt[idx]` when creating replacement connections, and call picoquic with the planned values.
- [ ] During metrics collection, count configured connections and read picoquic's `cwin_notified_from_seed` flag as the applied state.
- [ ] Render the new JSON fields and re-run `nb_metrics_test` until it passes.

### Task 4: Focused Verification and KZ Rollout

**Files:**
- Modify only if required by the existing rollout interface: `build/deploy_kz_startup_seed_controlplane.py`
- Create: `build/verify_kz_lane_seed.py`

- [ ] Run `go test ./internal/transportprofile`, `nb_transport_seed_test`, `nb_metrics_test`, and the line-count gate.
- [ ] Build the Linux release with the existing deployment build path and preserve its release identifier.
- [ ] Atomically roll out Exit, Middle, then Entry for KZ and wait for generation convergence.
- [ ] Read all KZ health/metrics sockets and require configured seeds to equal applied seeds on outbound workers; do not print credentials.
- [ ] Run a short KZ burst canary and compare Middle queue pressure-drop counters before and after. Require zero new drops and preserve FEC observe/adaptive settings.
