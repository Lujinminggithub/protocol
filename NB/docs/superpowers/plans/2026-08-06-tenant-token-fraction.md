# Tenant Token Fraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve fractional token refill across high-frequency QUIC callbacks.

**Architecture:** Add one per-direction fractional accumulator to local and shared tenant state. Use one
common refill helper so both paths implement identical saturation and overflow-safe arithmetic.

**Tech Stack:** C11, POSIX shared mmap, CMake/CTest.

---

### Task 1: Reproduce fractional refill loss

**Files:**
- Modify: `tools/test_tenant.c`

- [x] Add a 5 Mbps tenant and drain its 625,000-byte one-second burst.
- [x] Perform 2,000 one-microsecond `nb_tenant_take(..., 1, ...)` calls and require 1,250 bytes for both local and shared state.
- [x] Run `nb_tenant_test` and confirm the old implementation reports `RESULT FAIL`.

### Task 2: Preserve refill remainder

**Files:**
- Modify: `src/nb_tenant.h`
- Modify: `src/nb_tenant.c`
- Modify: `src/nb_tenant_shared.c`

- [x] Add `token_fraction[2]` to local and shared tenant state and bump the shared schema version to 3.
- [x] Add an overflow-safe helper that carries modulo 1,000,000 remainder between refills.
- [x] Use the helper in local and shared refill paths and clear the fraction when the bucket saturates.
- [x] Run `nb_tenant_test` and confirm `RESULT PASS`.

### Task 3: Build and deploy KZ

**Files:**
- No additional production files.

- [x] Build the full Linux data-plane release and run the focused tenant, queue, and metrics tests.
- [x] Atomically deploy Exit, Middle, and Entry, preserving KZ transport generation 4.
- [x] Read back all KZ workers and leave the line ready for a fresh live canary.
