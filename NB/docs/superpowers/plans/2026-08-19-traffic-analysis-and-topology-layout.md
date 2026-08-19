# Traffic Analysis And Persistent Topology Layout Plan

> Execute this plan on branch `V1.5.1`. Preserve existing production data, tokens, worker state, and data-plane deployments.

## Goal

Provide a full-screen traffic analysis workspace with exact time-range and event navigation, and persist operator-adjusted topology coordinates in SQLite so refreshes and browsers share one stable Entry-to-Exit layout.

## Task 1: Persist topology coordinates

1. Add failing store tests for topology layout save, readback, validation, and reset.
2. Add the `topology_layouts` migration and store methods.
3. Attach optional coordinates to topology devices and pass the tests.
4. Add failing HTTP tests for authenticated save/reset APIs.
5. Implement `PUT` and `DELETE /api/v1/topology/layout`, including finite/range/existing-device validation.

## Task 2: Complete traffic evidence markers

1. Add failing traffic-history tests for operations, operation events, incidents, deployment changes, and profile changes.
2. Extend the history query using safe labels only; never return task output, credentials, or raw incident payloads.
3. Pass focused central-store tests.

## Task 3: Build the full-screen traffic workspace

1. Add pure time-range helpers and Node tests for presets, custom range validation, and event-centered ranges.
2. Refactor traffic charts to support explicit `from`/`to`, live/history modes, and stacked fixed-height charts.
3. Add a full-screen in-console workspace with quick ranges, exact time inputs, event rail, event centering, and return-to-live action.
4. Replace the crowded line-detail charts with a compact latest-traffic summary and an “open traffic analysis” command.

## Task 4: Make topology stable and health-readable

1. Add pure layout helpers and Node tests for Entry-to-Relay-to-Exit ordering and saved-coordinate precedence.
2. Stop replacing graph objects and camera state on unchanged refreshes.
3. Persist drag-end coordinates through the new API with debounce and add reset-layout control.
4. Color the full computer model by health: normal neutral, warning amber, unhealthy/offline red.

## Task 5: Verify and publish

1. Run focused Go and Node tests, then `go test` and `go vet` for controlplane packages.
2. Build a local Windows web binary and verify desktop/mobile layout, nonblank canvases, no overlap, exact range behavior, event navigation, topology ordering, drag persistence, and health colors.
3. Commit the implementation.
4. Build Linux `nb-web`, atomically deploy only that service to HK, and preserve SQLite/tokens/worker/data.
5. Verify service health, database migration, public HTTPS, worker continuity, and no restart loop.
