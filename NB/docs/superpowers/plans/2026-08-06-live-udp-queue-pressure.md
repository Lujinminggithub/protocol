# Live UDP Queue Pressure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent a bounded media UDP queue burst from closing the entire live flow.

**Architecture:** Keep the existing raw datagram queue and average-rate shaper. Add a tested UDP helper that removes one complete oldest wire packet, then apply a media-only latest-data policy at the node queue boundary while preserving per-flow and instance memory caps.

**Tech Stack:** C11, CMake/CTest, picoquic, existing Python deployment tools.

---

### Task 1: Complete-packet queue eviction

**Files:**
- Create: `src/nb_udp_queue.h`
- Create: `src/nb_udp_queue.c`
- Create: `tools/test_udp_queue.c`
- Modify: `CMakeLists.txt`
- Modify: `tools/deploy.py`

- [x] Add a failing test that builds two two-fragment framed queue packets, calls `nb_udp_queue_drop_oldest_packet`, and expects both fragments of sequence 1 removed while sequence 2 remains.
- [x] Run the focused UDP queue build and confirm compilation fails because the helper is absent.
- [x] Implement `nb_udp_queue_drop_oldest_packet` with strict framing validation and a returned removed-byte count.
- [x] Re-run the UDP queue test and confirm it passes.

### Task 2: Media pressure semantics and observability

**Files:**
- Modify: `src/nb_metrics.h`
- Modify: `src/nb_metrics.c`
- Modify: `tools/test_metrics.c`
- Modify: `src/nb_session.h`
- Modify: `src/nb_node_session.inc`
- Modify: `src/nb_node_local.inc`
- Modify: `src/nb_node_transport.inc`

- [x] Add a failing metrics test expecting `queue_pressure_dropped` in the rendered `udp_errors` object.
- [x] Run the focused metrics build and confirm compilation fails because the metrics API is absent.
- [x] Add a saturating lifetime counter and render it in metrics JSON.
- [x] Add media-only preflight room creation for complete packets, soft-drop incoming packets when the instance budget cannot admit them, and keep hard failures fatal.
- [x] Treat FEC repair pressure as a soft repair drop and add the per-flow pressure count to UDP close logs.
- [x] Run the focused UDP queue and metrics tests and confirm both pass.

### Task 3: KZ build, deployment, and correlation check

**Files:**
- No source files beyond Tasks 1-2.
- Preserve diagnostics under `build/`.

- [x] Build the Linux data-plane release with the existing deployment build path.
- [x] Atomically deploy the same release to KZ Exit, Middle, and Entry without changing transport generation 4.
- [x] Read back health from all six KZ workers and confirm release and generation consistency.
- [ ] Run the user's live canary and confirm queue pressure may soft-drop complete stale media packets without increasing `closed.error` or producing `udp-down-queue-fail`.
- [x] Leave the line ready for the user's live canary and preserve all prior incident evidence.

### Task 4: Remove content-blind media expiry

**Files:** `src/nb_live.[ch]`, `src/nb_node_transport.inc`, `src/nb_node_main.inc`, `tools/test_live.c`

- [x] Add a failing assertion that media UDP expiry is disabled while control and bulk expiry remain enabled.
- [x] Run `nb_live_test` and confirm the failure is the missing policy API.
- [x] Add the policy helper and guard only the two datagram expiry call sites; preserve capacity limits and complete-packet pressure eviction.
- [x] Re-run `nb_live_test`, `nb_udp_queue_test`, and the 1,000-line source gate.
- [x] Build on GZ and atomically activate one binary on KZ Exit, Middle, and Entry without changing generation 4 or FEC state.
- [ ] Require the next short canary to show zero media deadline drops and no unexplained packet gap.
