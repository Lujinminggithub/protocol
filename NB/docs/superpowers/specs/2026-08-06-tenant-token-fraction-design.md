# Tenant Token Fraction Design

## Problem

The tenant token buckets convert microseconds to bytes with integer division and then always advance
the refill timestamp. At 5 Mbps, callbacks one microsecond apart each produce 0 whole bytes, so the
fractional 0.625 byte is discarded forever. Once the initial burst is consumed, valid traffic below
the configured average can be starved and its media queue expires or fills.

## Design

Store a remainder in millionths of a byte for each direction in both the process-local tenant record
and the cross-worker shared tenant record. Refill with `elapsed_us * rate_bytes_per_sec + remainder`;
move the quotient into tokens and retain the modulo 1,000,000 remainder. Saturation at the burst cap
clears the remainder because unused capacity must not accumulate beyond the configured burst.

The shared mmap schema version changes from 2 to 3 so old runtime-only state is reinitialized safely.
No tenant configuration, credentials, accounting database, queue limits, FEC profile, or transport
generation changes.

## Verification

The tenant test drains a 5 Mbps one-second burst and performs 2,000 one-microsecond refill/take calls.
Both local and shared buckets must deliver exactly 1,250 bytes. The old implementation delivers zero.
After deployment, KZ must retain generation 4 and all workers must expose the new release. A live
canary must show no new queue pressure or deadline drops on the principal upload flow.
