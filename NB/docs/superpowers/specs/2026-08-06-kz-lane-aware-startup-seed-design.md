# KZ Lane-Aware Startup Seed Design

## Problem

The KZ live canary received 7.6-9.37 Mbps during its first ten seconds. One HK middle shard used a QUIC path near 281 ms, but generation 5 configured a 202.757 ms BDP seed derived from ICMP. Picoquic only accepts a seed within 25 percent of the first RTT sample, so that lane rejected the seed. Its 256 KiB media queue reached 262143 bytes and dropped 3710 packets, exactly matching the packet deficit at the exit.

The 5 Mbps package value is an average service and billing rate. It must not be treated as the maximum startup burst rate.

## Design

### Control Plane Default

For a long-haul BBR segment, Go uses the loaded QUIC RTT P95 as `seed_rtt_us`. The startup window remains a two-times-average-rate BDP, calculated with that QUIC RTT and rounded up to 64 KiB. `target_rate_bps` remains the committed average rate for accounting and reporting.

Short CUBIC segments retain their current congestion-control and bounded-window behavior.

### Lane-Aware Runtime Override

When make-before-break creates a replacement connection, the C runtime supplies the selected pool lane's recent RTT to a focused seed planner. If the sample is valid, the planner replaces the profile RTT and scales the profile startup window by `observed_rtt/profile_rtt`, rounded up to 64 KiB and bounded to 256 KiB through 64 MiB. A cold process without a lane sample uses the Go-generated profile values.

The queue remains capped at 256 KiB. Adaptive FEC settings remain unchanged.

### Observability

Each outbound connection records the effective seed RTT and window. Metrics expose the number of configured and picoquic-confirmed applied seeds, plus maximum effective RTT and window. Deployment verification requires every outbound KZ worker to report at least one configured seed and `applied == configured` after replacement connections become ready.

### Verification

Use test-first coverage for Go calculation, C lane scaling, and metrics JSON. Run only the related Go and C tests, the repository line limit gate, a KZ build, a short burst canary, and three-node health/metrics readback. The live canary must show no new Middle queue pressure drops.
