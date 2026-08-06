# Live UDP Queue Pressure Design

## Problem

The 08:52 KZ live test produced 52 `udp-down-queue-fail` closures when transient media bursts reached the 256 KiB per-flow limit. The 11:45 test exposed a second failure: transport loss and route outages were both zero, but Middle expired 12 queued wire fragments at the fixed 200 ms deadline. Those fragments account exactly for the six original packets missing between Middle input (8,374) and Exit output (8,368). They were removed from the first media burst while the queue used only about 62 KiB of its 256 KiB bound.

## Decision

Keep send-boundary average-rate enforcement and all memory limits. Media UDP uses one bounded pressure policy:

- Do not remove media records solely because they have waited 200 ms. NB cannot identify codec frames or dependency boundaries, so such deletion can corrupt the first keyframe.
- When a complete new media packet would exceed 256 KiB per flow, remove complete oldest logical packets until it fits. Never remove one fragment.
- If the 64 MiB instance budget still cannot admit it, discard the complete incoming packet without closing the flow.
- Allocation, connection, encoding, and QUIC API failures remain fatal.
- FEC repair packets may be discarded under pressure but must not evict source media.
- Preserve cumulative `queue_pressure_dropped` metrics and per-flow close evidence.

This is not an unbounded queue. The 256 KiB per-flow and 64 MiB per-instance limits remain authoritative. Control and bulk expiry behavior, FEC thresholds, and transport profiles are unchanged.

## Validation

Add a C policy test proving media UDP expiry is disabled while control and bulk expiry stay enabled. Run only `nb_live_test`, `nb_udp_queue_test`, the source line gate, and the Linux data-plane build on GZ. Deploy one binary to KZ Exit, Middle, and Entry, then verify all six workers keep transport generation 4 and FEC inactive. The next short live canary must show zero media deadline drops, zero no-path events, and no unexplained cross-role packet gap.
