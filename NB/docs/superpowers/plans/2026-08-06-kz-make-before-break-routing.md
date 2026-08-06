# KZ Make-Before-Break Routing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve UDP route availability while a single-slot downstream connection is being replaced.

**Architecture:** Delay quarantine until transport progress has stalled, then use a healthy-first selector with the quarantined active connection as a final fallback. Keep replacement promotion and draining unchanged.

**Tech Stack:** C11, CMake/CTest, picoquic, Python atomic deployment tooling

---

### Task 1: Reproduce the state and selection failures

**Files:**
- Modify: `tools/test_pool_health.c`
- Test: `tools/test_pool_health.c`

- [ ] Add assertions that queue degradation with delivery progress does not quarantine and that a single available quarantined slot remains selectable.
- [ ] Build and run `nb_pool_health_test`; verify the new assertions fail against current behavior.

### Task 2: Correct health and routing semantics

**Files:**
- Modify: `src/nb_pool_health.h`
- Modify: `src/nb_pool_health.c`
- Modify: `src/nb_node_pool.inc`

- [ ] Change health evaluation so quarantine requires the configured progress stall condition.
- [ ] Add a pure healthy-first slot selector that falls back to an available quarantined slot.
- [ ] Use the selector in all round-robin fallback paths without changing quality ranking or replacement promotion.
- [ ] Rebuild and run `nb_pool_health_test`; expect `RESULT PASS`.

### Task 3: Verify and deploy KZ

**Files:**
- Create: `build/deploy_kz_mbb_routing_fix.py`

- [ ] Run the source line-count gate and the pool health unit test.
- [ ] Build the Linux node from the current source fingerprint.
- [ ] Atomically deploy the shared binary and reload only the KZ instance.
- [ ] Read back KZ Entry, Middle, and Exit health; require the same new binary release, generation 4, and healthy status.
- [ ] During the next short live canary, require zero new `udp-middle-no-path` events.
