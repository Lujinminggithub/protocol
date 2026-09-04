# Media Queue and Exit Recovery Design

## Goal

Remove NB-induced live-video discontinuities under short upstream bursts while preserving the 5 Mbps service rate, and make Exit DNS/TCP failures observable and bounded so TikTok recovery requests cannot remain stuck on unreachable private addresses.

## Evidence

- During the 2026-09-04 11:48 CST stream, Entry accepted short media bursts near 12 Mbps while the service rate remained 5 Mbps.
- The Entry media datagram queue reached its fixed 262144-byte limit. The current media-room path evicts the oldest complete UDP packet at that limit and increments `queue_pressure_dropped`.
- UDP flow logs and metrics report queue age as zero because they use TCP ring lengths even when the active queue is a timestamped datagram queue.
- Media delivery fell from 3.66 Mbps to 0.36 Mbps, switched from port 50001 to 50020, and later fell through 1.54, 1.09, 0.48, and 0.85 Mbps before recovering.
- A 60-second post-event sample showed no continuing queue-pressure or receive-overflow growth, so the fix must target short bursts rather than sustained loss.
- `2fio72ng.sg-fn.tiktok-row.net` resolves to `10.105.212.98` and `10.105.212.204` through the system resolver, direct UDP queries to 1.1.1.1/8.8.8.8, and Cloudflare/Google DoH. TCP port 443 on both addresses is unreachable from Exit.
- Normal `live-netacc` and `rtc-access` Exit connections complete in approximately 62-126 ms. Private-answer handling must therefore be narrowly scoped and must not affect normal TikTok endpoints.

## Scope

This change includes:

- dynamic per-flow media datagram queue capacity;
- correct datagram queue age reporting;
- rate-limited queue-pressure diagnostics;
- a 2,500,000-byte upstream tenant burst with the sustained rate unchanged at 5 Mbps;
- Exit DNS latency/address/result instrumentation;
- fast rejection of RFC1918 A records for `*.tiktok-row.net`;
- a five-second Exit TCP connect deadline;
- counters and logs for DNS failure, private-answer rejection, connect failure, and connect timeout.

This change does not include:

- selective Entry-to-Exit signal routing;
- changes to the 5 Mbps sustained upstream/downstream rates;
- forced FEC;
- changes to the Guangzhou-Hong Kong dedicated media path;
- synthetic public-IP substitution for a private TikTok DNS answer;
- an attempt to solve the separate 400-600 ms signal RTT in this release.

## Media Queue Design

The queue limit is selected per flow and per direction from the active transport link:

```text
window_us = max(reorder_delay_us, 200000)
bdp_bytes = target_rate_bps * window_us / 8 / 1000000
media_queue_limit = ceil_64k(clamp(2 * bdp_bytes, 262144, 1048576))
```

For a 5 Mbps target and a 462 ms reorder window, the result is 589824 bytes. Missing or invalid transport rate information falls back to the current 262144-byte limit.

Control and bulk datagram queues retain the existing 262144-byte limit. Media eviction continues to remove one complete original UDP packet, including all of its fragments, so the queue never retains partial packet groups. Media UDP deadline expiry remains disabled; the hard capacity guard is the only media eviction point.

The tenant rate remains 5000 Kbps upstream and downstream. The upstream burst changes from 625000 to 2500000 bytes, while the downstream burst remains 625000 bytes. This absorbs short camera bursts without changing long-term billing behavior.

## Queue Observability

The datagram record already carries `queued_at`. A queue helper will expose the age of the oldest complete record without mutating the queue. UDP flow logs and the metrics snapshot will use this helper for `udp_down_tx`, `udp_up_tx`, and `udp_pending_tx` instead of returning zero.

When media-room eviction occurs, NB will emit a rate-limited INFO record containing role, flow ID, target, dropped packet count, queue bytes, queue limit, and oldest age. Counters remain monotonic and continue to count complete original UDP packets.

## Exit DNS and Connect Design

Each asynchronous DNS result will carry the original host, submission timestamp, completion timestamp, selected address, and outcome. Exit logs will report DNS latency and the selected numeric address without logging credentials or payloads.

If a hostname ending in `.tiktok-row.net` resolves to an RFC1918 IPv4 address, Exit will classify the result as `private-unreachable`, increment a dedicated counter, reset the upstream stream, and release the flow immediately. The rule does not apply to IP-literal targets or other domains, preserving intentional private destinations outside this TikTok-specific recovery case.

Nonblocking TCP connect remains the normal path for usable addresses. A five-second deadline is added to the event-loop deadline calculation. A flow still connecting at the deadline is reset with reason `target-connect-timeout`, and the timeout and maximum observed connect latency are exported in metrics.

DoH is not added because two independent DoH providers return the same private records; changing resolver transport cannot produce a usable address.

## Failure Handling

- Allocation failure retains the existing queue and teardown behavior.
- A direct DNS failure, private TikTok-row answer, connect start failure, socket completion failure, or connect timeout has a distinct close reason and counter.
- The Exit state machine closes the local socket and resets the upstream stream exactly once.
- Metrics additions are monotonic and do not change scheduling decisions.
- Deployment uses the existing shared-binary session drain and atomic rollback. Any failed role activation restores the current deployment and transport generation 2.

## Testing

Unit tests will cover:

- queue limit calculation at the 5 Mbps/462 ms boundary and both clamps;
- oldest datagram age for empty, queued, consumed, and malformed queues;
- whole-packet eviction at the dynamic limit;
- private-address classification limited to TikTok-row hostnames;
- DNS timing/result propagation;
- five-second connect-deadline decisions;
- metrics rendering for all new counters.

Integration verification will include:

- the full remote CMake/CTest build gate;
- the existing three-hop integrity and UDP lifecycle smoke tests;
- a 12 Mbps, 500 ms media-burst test with zero queue-pressure drops at the 5 Mbps sustained rate;
- a private TikTok-row DNS result completing as fast failure rather than a TCP timeout;
- normal live-netacc/rtc-access DNS and TCP behavior remaining unchanged;
- two-worker health, binary hash, transport generation, 5 Mbps rates, and tenant burst readback;
- a timed phone broadcast plus viewer-side continuity check.

## Rollout

1. Apply and verify the upstream burst-only tenant update.
2. Build the new shared binary and run the complete test gate on Guangzhou2.
3. Wait for active sessions to drain.
4. Deploy Exit, Middle, and Entry transactionally.
5. Verify all six line controls and current transport generation 2.
6. Run synthetic media-burst and Exit recovery probes.
7. Run a timed phone broadcast and compare queue-pressure deltas, media rate continuity, and viewer behavior.
8. Roll back to the current deployment if any gate fails or viewer continuity regresses.
