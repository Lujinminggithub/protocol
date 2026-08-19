# Force Delete Unreachable Line Design

## Problem

`line.disable` requires SSH access to every data-plane node. When an exit is permanently unreachable, the operation fails and the line remains `active`. The UI only exposes deletion for draft, disabled, or archived lines, while the store rejects deletion of a configured active line. This creates a deadlock: the operator cannot stop or delete the line.

## Decision

Add an explicit, administrator-only force-delete path for a line whose normal shutdown cannot complete.

- Normal deletion keeps the existing requirement that configured lines are disabled first.
- Force deletion bypasses only the line-status requirement.
- Force deletion still rejects queued, dispatched, or running operations.
- The operator must enter the exact line ID, provide a reason, and acknowledge that unreachable nodes may retain remote processes or files.
- The deletion audit records the force flag, confirmation, line snapshot, resource counts, and latest device health.
- The UI shows `强制删除` on configured lines that cannot use normal deletion. It uses a dedicated confirmation dialog and never silently falls back from normal deletion.
- The operation is a control-plane deletion. It does not claim that unreachable remote resources were stopped.

## API

`DELETE /api/v1/lines/{id}` accepts:

```json
{
  "requested_by": "operator",
  "reason": "KZ exit permanently unavailable",
  "force": true,
  "confirmation": "gz-hk-kz-00001",
  "acknowledge_orphans": true
}
```

For force deletion, `confirmation` must exactly match `{id}` and `acknowledge_orphans` must be true. Validation errors and conflicts are returned in Chinese.

## Safety And Evidence

An active operation remains a hard conflict. A force deletion is atomic in SQLite and writes its audit record before deleting line-owned rows. Device health is included in the audit so the reason for bypassing shutdown remains reviewable.

## Verification

Tests cover normal deletion rejection, confirmation validation, active-operation rejection, successful force deletion, retained deletion audit, and UI exposure of the force-delete action.
