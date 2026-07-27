# NB operations control plane

`nb-control` keeps users, credentials, quota and usage events in SQLite and
reconciles the C node's `users.conf` and `tenants.conf`. It also collects worker
health/metrics and sends durable, idempotent events to an existing web service.

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
