# Control Plane Public Access Design

## Goal

Allow operators from any source IP to open the HTTPS control-plane endpoint while retaining administrator-token authorization for management APIs.

## Design

- Nginx listens publicly on TLS port 9091 without source-IP `allow` or `deny` directives.
- `nb-web` remains bound to `127.0.0.1:19091`, so it cannot be reached directly from the network.
- Nginx continues to proxy all requests to the loopback listener and preserves the existing TLS configuration.
- `nb-web` remains responsible for administrator and agent bearer-token authorization. This change does not enable anonymous management API access.
- The repository Nginx template is updated together with the production site configuration so future deployments do not restore the old workstation-IP restriction.

## Deployment Safety

Back up the active Nginx site, stage the unrestricted configuration, validate it with `nginx -t`, atomically activate it, and reload Nginx. Restore the backup if validation or reload fails.

## Verification

- The repository regression test rejects source-IP `allow` and `deny` directives.
- Nginx configuration validation succeeds.
- Public HTTPS `/healthz` is reachable without a source-IP 403.
- A management API request without a token remains unauthorized.
- A management API request with the administrator token succeeds.
- `nb-web` remains bound only to `127.0.0.1:19091`.
