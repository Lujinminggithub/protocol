# KZ Make-Before-Break Routing Design

## Evidence

During the 2026-08-06 10:29:50 CST KZ live canary, Middle started a
replacement at 10:29:31, rejected 220 new UDP flow attempts with
`udp-middle-no-path` from 10:29:44, and promoted the replacement only at
10:31:00. The pool has one slot. Its lifetime metrics recorded two
quarantines and one make-before-break promotion.

The active selector skips every quarantined slot. Health evaluation marks a
slot quarantined as soon as its media queue is degraded, even while ACK
delivery continues. These two rules turn a recoverable queue spike into an
availability gap before the replacement is ready.

## Design

Health state has two stages:

1. A queue/transport degradation with continuing ACK progress is recorded as
   degraded but remains routable.
2. Quarantine begins only after delivery has stopped for the configured stall
   grace period. Replacement creation and promotion retain the existing hold,
   cooldown, and drain rules.

Selection uses two passes. It first selects a non-quarantined active
connection. If none exists but an active connection still exists, it selects
the quarantined connection as the least-bad fallback until replacement
promotion completes. A missing connection still produces `no-path`.

This preserves single-slot availability without disabling recovery. It does
not change FEC, tenant rates, queue sizes, congestion control, or US runtime
configuration.

## Verification

Add pure unit coverage for delayed quarantine and healthy-first/degraded-
fallback selection. Run only the pool health test and the existing P1 line
count gate. Build the Linux node, deploy the shared binary atomically, reload
only the KZ instance, and verify both KZ Middle workers report the new binary,
generation 4, and healthy status. A short live canary must show no new
`udp-middle-no-path` events.
