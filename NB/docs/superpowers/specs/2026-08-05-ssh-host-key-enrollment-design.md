# SSH Host Key Enrollment Design

## Goal

Require an explicit operator-confirmed SSH host key for every managed device, persist that identity in the central database, and make the worker reject an untrusted device before any line build, upload, or deployment starts.

## Trust Model

Device registration is a two-stage flow. The authenticated administrator asks the Web service to perform a bounded SSH handshake against the entered host and port. The service returns the SSH key algorithm and SHA256 fingerprint without trusting or persisting it. The UI displays those values and requires an explicit confirmation. The final device upsert includes the scanned public key and fingerprint; the server parses the key, recalculates the fingerprint, and rejects mismatches.

This is explicit TOFU. It protects against accidental key changes and silent `AutoAddPolicy`, but the operator must compare the displayed fingerprint with an independent value supplied by the server owner before confirmation.

## Persistence

The `devices` table gains `ssh_host_key`, `ssh_host_key_type`, `ssh_host_key_sha256`, `ssh_host_key_status`, and `ssh_host_key_confirmed_at`. Existing rows migrate to `pending`. A device is deployable only when status is `trusted` and the stored key is internally consistent.

Updating a device without changing its SSH host and port preserves the trusted identity. Changing host or port requires a newly scanned and confirmed key. Public host keys and fingerprints may be returned by the device API; passwords remain in the existing private secret store and are never returned.

## Worker Trust Store

The central operation plan already embeds the selected devices. It will also carry their confirmed public host keys. Before resolving secrets or writing deployment inputs, the worker validates all three nodes. Missing or invalid trust data returns `设备 <id> 尚未完成 SSH 主机密钥登记` during the prepare stage.

For a valid plan, the worker renders canonical OpenSSH known-host entries using `host` for port 22 and `[host]:port` otherwise. It atomically replaces the per-line `security/known_hosts` file with exactly the selected nodes. Dynamic lines no longer inherit stale global entries. Python deployment continues using `RejectPolicy`; insecure mode remains limited to explicit bootstrap tools.

## UI Flow

Submitting the device form first scans the endpoint. A confirmation panel displays host, port, key algorithm, and SHA256 fingerprint. Only the confirm action sends the device upsert. Editing a device at the same endpoint may retain its trusted key; editing the endpoint always triggers a new scan and confirmation.

## Failure Handling

Scan timeout, malformed SSH handshakes, unsupported keys, fingerprint mismatches, and unconfirmed keys use Chinese operator-facing errors. No key is saved on a failed scan or cancelled confirmation. Opening a line with any pending device fails in prepare, before `operation prepared` and before Python is launched.

## Verification

Tests cover SQLite migration, fingerprint validation, endpoint-change invalidation, scan API behavior with an in-process SSH server, UI confirmation behavior, worker preflight rejection, canonical nonstandard-port rendering, atomic replacement, and absence of `AutoAddPolicy` in the production deployment path.
