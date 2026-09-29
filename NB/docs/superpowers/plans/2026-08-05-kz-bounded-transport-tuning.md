# KZ Bounded Transport Tuning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the KZ reorder-tuning ratchet with bounded evidence-derived parameters and decouple live UDP queue expiry from QUIC loss-reordering tolerance.

**Architecture:** Python emits robust schema-2 evidence, Go is the sole production parameter calculator, the worker keeps the existing transactional rollout, and C applies transport generations per connection while enforcing an independent real-time UDP deadline.

**Tech Stack:** Python 3, Go, C/CMake, existing NB control-plane API and KZ SSH deployment tooling.

---

### Task 1: Robust probe evidence and removable candidates

**Files:**
- Modify: `tools/test_line_probe.py`
- Modify: `tools/line_probe.py`
- Modify: `tools/test_line_control.py`
- Modify: `tools/nb_line_control.py`

- [ ] Add a failing probe test showing clean qualified evidence can reduce a 450 ms current allowance and that a transient maximum does not replace the robust percentile.
- [ ] Run `python tools/test_line_probe.py` and confirm the new assertion fails because the current allowance is preserved.
- [ ] Emit reorder P95 fields and calculate bounded advisory values without the qualified-current floor; preserve current values only for insufficient load.
- [ ] Add a failing legacy approval test showing a qualified reduction is accepted, remove the no-decrease checks, and run both Python tests to green.

### Task 2: Go-authoritative bounded transport generation

**Files:**
- Modify: `controlplane/internal/transportprofile/profile_test.go`
- Modify: `controlplane/internal/transportprofile/profile.go`

- [ ] Replace the candidate-preservation test with a failing KZ regression asserting long-hop delay at most 120 ms and gap at most 64 despite a 573 ms advisory candidate.
- [ ] Add a failing test proving physical RTT is not added to reorder delay and a load-qualified clean sample may generate a lower profile.
- [ ] Run `go test ./internal/transportprofile` and confirm failures come from preserving the advisory candidate.
- [ ] Add percentile evidence fields, calculate bounded links in Go, and make raw counts/windows determine qualification.
- [ ] Run `go test ./internal/transportprofile` and related Web tests to green.

### Task 3: Independent live UDP deadline

**Files:**
- Modify: `tools/test_live.c`
- Modify: `src/nb_live.c`

- [ ] Change the C test to require a 200 ms media UDP deadline for 573 ms and 900 ms reorder tolerances.
- [ ] Build/run `nb_live_test` and confirm the old path-derived deadline fails.
- [ ] Make `nb_live_queue_limits_for_path` keep the base real-time deadline independent of reorder tolerance.
- [ ] Rebuild/run the C test and relevant C gates to green.

### Task 4: Build and KZ-only deployment

**Files:**
- Build: `build/nb_node`
- Build: `build/release-manifest.json`
- Build: `build/kz-rootcause-controlplane/nb-web`
- Build: `build/kz-rootcause-controlplane/nb-web-worker`

- [ ] Run focused Python, Go, and C tests plus source line-count checks.
- [ ] Build the Linux C and Go artifacts using the repository's existing build paths.
- [ ] Atomically deploy the HK control plane and repository, preserving service readiness.
- [ ] Upgrade only `gz-hk-kz-00001`, read back release and health on KZ Entry/Middle/Exit, and leave US untouched.

### Task 5: KZ validation, generation rollout, and short canary

**Files:**
- Evidence: `build/` generated validation and rollout records only.

- [ ] Trigger a fresh KZ validation and require schema 2, at least 10,000 packets and six valid windows per segment.
- [ ] Trigger protocol tuning and require bounded KZ parameters, transaction success, and a consistent non-zero generation across all KZ workers.
- [ ] Confirm FEC observe/adaptive is enabled while forced active remains disabled.
- [ ] Run only passive/related machine checks, then hand off for the user's live-stream canary without starting a new 24-hour soak.
