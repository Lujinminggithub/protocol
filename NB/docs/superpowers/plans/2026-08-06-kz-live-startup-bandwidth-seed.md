# KZ Live Startup Bandwidth Seed Implementation Plan

1. Add failing Go assertions for target rate, seed RTT, startup BDP, and role
   profile rendering.
2. Add failing C parser assertions for the new link directives and invalid
   bounds.
3. Extend the Go link model and generator with integer runtime fields.
4. Render, parse, validate, and retain the runtime fields in C.
5. Apply the validated seed to new outbound BBR connections before the QUIC
   client starts.
6. Run focused tests, build the data plane, and enforce the 1,000-line limit.
7. Deploy a new KZ transport generation, read it back, and collect a short
   canary without enabling active FEC.
