# KZ Live Startup Bandwidth Seed Design

## Scope

Fix the deterministic packet loss observed during the 2026-08-06 13:22
KZ live startup without increasing the per-flow UDP queue or enabling active
FEC.

## Evidence

- Entry accepted 9,656 media UDP packets (11,020,466 bytes).
- Middle accepted the same 9,656 packets, so GZ to HK was lossless.
- Middle egress reached the 256 KiB queue bound and pressure-evicted 1,629
  complete packets. Deadline eviction was zero.
- Exit received 8,019 packets (9,072,352 bytes).
- The HK to KZ connection delivered about 4.9 Mbps in its first five-second
  window and about 9.6 Mbps in the next one.
- The line contract is 5 Mbps, but provisioning grants ten seconds of token
  credit. That permits a roughly 10 Mbps startup burst while the long-RTT BBR
  connection still has a cold bandwidth estimate.
- The Go profile already computes `target_mbps`, but the role profile renderer
  and C parser discard it.

## Decision

The transport profile will carry three integer directives per link:

- `target_rate_bps`: committed average service rate.
- `seed_rtt_us`: robust physical path RTT from probe evidence.
- `startup_cwin_bytes`: a bounded startup BDP computed for a two-times live
  peak, rounded to 64 KiB and capped at 64 MiB.

The Go control plane remains the policy owner. The worker keeps the existing
transactional Exit, Middle, Entry rollout. The C data plane validates and
stores the directives. When Entry or Middle creates a new outbound BBR
connection for the active generation, it calls picoquic's existing validated
BDP seed API before starting the connection. Picoquic applies the seed only
after the first RTT sample matches the configured RTT within 25 percent and
the peer IP matches, which prevents a stale profile from forcing an unrelated
path.

The seed is a startup floor, not a permanent pacing override. Congestion
control continues to reduce its rate when the path reports loss or delay. The
tenant token bucket continues to enforce the 5 Mbps long-term average. The
256 KiB media queue and 64 MiB instance pressure bounds remain unchanged.

## Rejected Alternatives

- Reducing the ten-second burst alone moves loss to Entry and cannot preserve
  a UDP live peak.
- Increasing the Middle queue hides the mismatch behind latency and memory.
- Always-on FEC spends bandwidth after local queue loss has already occurred.

## Verification

1. Go tests prove profile generation and role rendering preserve all three
   directives and calculate a 2x bounded startup BDP.
2. C tests prove parsing, bounds, duplicate detection, and compatibility of
   the new directives.
3. Build the current tree and run focused transport/UDP queue tests plus the
   1,000-line gate.
4. Roll a new KZ generation transactionally and read back the same generation
   on all six workers.
5. A short production canary must show zero Middle pressure eviction for the
   live flow. Active FEC remains off unless measured network loss independently
   crosses policy.
