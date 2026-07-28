# NB operations control plane

`nb-control` keeps users, credentials, quota and usage events in SQLite and
reconciles the C node's `users.conf` and `tenants.conf`. It also collects worker
health/metrics and sends durable, idempotent events to an existing web service.

## Windows central operations service

`nb-web` is the central multi-line service. It runs on the Windows operations
host, embeds its browser UI in the Go binary, and keeps line metadata, latest
worker snapshots, incidents and typed operations in a separate SQLite database.
It never stores SSH passwords or node private keys; a line stores only a
`secret_ref` that points to the external secret owner.

Start it locally:

```powershell
$env:NB_WEB_ADMIN_TOKEN = "use-a-long-random-admin-token"
$env:NB_WEB_AGENT_TOKEN = "use-a-different-long-agent-token"
./start-nb-web.ps1 -Listen "127.0.0.1:9091"
```

Open `http://127.0.0.1:9091` and enter the admin token. The central API uses
`Authorization: Bearer`; all operation mutations additionally require an
`Idempotency-Key`. Supported operation types are `line.open`, `line.validate`,
`line.upgrade`, `line.rollback`, and `line.disable`. Agents can only claim typed
operations and return a terminal result, so the web service cannot execute
arbitrary SSH commands.

Existing Linux `nb-control` instances can report to the central service without
changing their collector payload:

```ini
NB_WEB_BASE_URL=http://windows-operations-host:9091
NB_WEB_TOKEN=<same value as NB_WEB_AGENT_TOKEN>
```

Register the matching `line_id` in the UI before enabling delivery. The central
service accepts the existing `/api/nb/v1/node-snapshots` and
`/api/nb/v1/incidents` outbox paths. New agents can use `/agent/v1/snapshots`,
`/agent/v1/incidents`, `GET /agent/v1/operations?line_id=...`, and
`POST /agent/v1/operations/{id}/result` directly.

SQLite WAL is appropriate for one Windows central process and dozens of lines.
Before running multiple central replicas, move the central store to PostgreSQL;
the per-line agent outbox remains SQLite.

### Windows operation worker

`nb-web-worker` is a separate Windows process. It polls the central service and
executes only these typed operations: `line.open`, `line.validate`,
`line.upgrade`, `line.rollback`, and `line.disable`. The worker never accepts a
host, credential, file path, or shell command from an operation payload.

Server placement is selected by `line_id` in the worker's ignored private
registry (`tools/private/nb-web-worker.json`). Each entry maps a line to a fixed
resource group, machine inventory, line profile, `known_hosts`, security
directory, and an explicit operation allowlist. The mapping is the authority
for where a line is deployed; the web line record is display and scheduling
metadata only. This permits multiple lines while keeping credentials outside
the central database.

Create the private registry from
`controlplane/nb-web-worker.registry.example.json`, then start the worker:

```powershell
$env:NB_WEB_BASE_URL = "http://127.0.0.1:9091"
$env:NB_WEB_AGENT_TOKEN = "use-the-central-agent-token"
./start-nb-web-worker.ps1
```

The worker sends a heartbeat with its per-line capabilities. Until an online
worker advertises the requested operation, the API rejects operation creation
and the UI disables the action with a `waiting for executor` explanation. A
worker writes the operation result to its local state directory before
acknowledging completion, which prevents a lost HTTP response from rerunning a
completed deployment.

Lines sharing GZ/HK use isolated deployment namespaces. The registry allocates
an `instance_id`, SOCKS port, Entry UDP relay range, Relay listener port, and
Exit listener port for every line in the resource group. Startup rejects port
or UDP-range collisions. The legacy/default namespace can keep the existing US
line on `1080/4443`; the KZ line can run as instance `kz` on SOCKS `1081`, Entry
UDP `21024-22047`, and Relay `4444`. The worker passes these allocations to the
atomic deployment tools. Windows remains the orchestration plane and is never
part of packet forwarding.

Incorrect or superseded draft lines are marked `archived`. Their historical
operations remain queryable, while the line is excluded from the active line
list, dashboard totals, and capacity calculations.

Build and install on the Entry host:

```bash
go build -trimpath -o nb-control ./cmd/nb-control
install -m 0755 nb-control /usr/local/bin/nb-control
install -m 0644 nb-control.service /etc/systemd/system/nb-control.service
install -m 0600 nb-control.env.example /etc/NB/nb-control.env
systemctl daemon-reload
systemctl enable --now nb-control
```

Production deployment uses `tools/controlplane_config.py` to generate ignored
credentials and `tools/deploy_controlplane.py` to install with hash checking,
health verification, a deployment lock and automatic restoration.

The API listens on loopback by default and requires `Authorization: Bearer` for
all `/v1/` endpoints. Mutations require an `Idempotency-Key`. User, credential,
audit and web-outbox records commit in one SQLite transaction. The web receiver
must accept the same idempotency key on these paths:

- `/api/nb/v1/provision-results`
- `/api/nb/v1/usage-events`
- `/api/nb/v1/node-snapshots`
- `/api/nb/v1/incidents`
- `/api/nb/v1/billing-periods`

Set `NB_WEB_SIGNING_KEY` to add `X-NB-Signature: sha256=<hex HMAC>` to every
outbound event. Prometheus scrapes `/metrics`; the supplied rules and Grafana
dashboard aggregate user, usage and delivery health across lines.
