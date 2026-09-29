# KZ Bounded Transport Tuning Design

## Goal

Restore KZ live-stream stability without reverting to an old profile or forcing FEC active. The controller must derive bounded transport parameters from raw link evidence, the worker must roll them out transactionally, and the C data plane must keep real-time UDP queue expiry independent from QUIC loss-reordering tolerance.

## Confirmed Root Cause

The current probe candidate uses the maximum observed reorder sample and refuses to reduce the current `reorder_gap` or `reorder_delay_us`. The Go controller then preserves that candidate, and the legacy approval path independently rejects reductions. This creates a one-way ratchet from transient probe noise to production values such as 573 ms.

The C live scheduler also extends the media UDP queue deadline from the same reorder delay. A high loss-detection tolerance can therefore retain stale live packets for more than 600 ms even though the real-time queue should expire independently.

## Design

1. `line_probe.py` continues to produce schema-2 evidence, but adds robust reorder percentiles and does not preserve a qualified current reorder allowance. Insufficient-load evidence still keeps the current configuration.
2. Go `transportprofile` is authoritative for parameters. It ignores advisory candidate transport values once raw evidence is load-qualified.
3. Reorder parameters use robust evidence and jitter, not physical RTT:
   - gap: `ceil(reorder_gap_p95) + 8`, bounded to 8..64;
   - delay: `reorder_delay_p95 + 3 * jitter_p95`, with a 20 ms floor;
   - short segment cap: 80 ms when physical RTT is at most 30 ms;
   - long segment cap: 120 ms otherwise.
4. Old schema-2 evidence without percentile fields remains readable. Its max fields are used as fallback but are still bounded, preventing another 573 ms generation.
5. A qualified clean validation may reduce an existing profile. The obsolete legacy no-decrease checks are removed.
6. Live UDP queue expiry remains 200 ms regardless of reorder tolerance. QUIC retains its own per-connection reorder tolerance.
7. Adaptive FEC stays available with `hold_us=2000`, observe enabled and forced active disabled. This change does not use FEC to hide delayed loss recovery.

## Deployment

Build the C node and Go control-plane binaries from the current worktree. Deploy the control plane to HK, then upgrade only the KZ line so US processes remain on their current release. Run a fresh KZ validation, generate a new profile generation, transactionally apply Exit then Middle then Entry, and confirm generation readback on all KZ workers.

## Acceptance

- Relevant Python, Go, and C tests pass.
- Generated KZ long-hop reorder delay is at most 120 ms and gap at most 64.
- Media UDP queue deadline remains 200 ms for any reorder tolerance.
- KZ Entry, Middle, and Exit report the same non-zero new transport generation.
- FEC remains observe/adaptive with forced active disabled.
- A short machine canary is healthy; the user then performs the live-stream canary.
