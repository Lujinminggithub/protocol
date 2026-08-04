#!/usr/bin/env bash
set -euo pipefail

root=/opt/nb-controlplane
for path in \
  "$root/bin/nb-web" "$root/bin/nb-web-worker" \
  "$root/etc/nb-web.env" "$root/etc/nb-web-worker.env" \
  "$root/etc/worker-registry.json" \
  "$root/etc/known_hosts"; do
  test -s "$path" || { echo "required private deployment file is missing: $path" >&2; exit 1; }
done

chmod 0755 "$root/bin/nb-web" "$root/bin/nb-web-worker"
chmod 0600 "$root/etc/nb-web.env" "$root/etc/nb-web-worker.env" \
  "$root/etc/worker-registry.json" "$root/etc/known_hosts"
install -m 0644 "$root/repo/controlplane/linux/nb-web.service" /etc/systemd/system/nb-web.service
install -m 0644 "$root/repo/controlplane/linux/nb-web-worker.service" /etc/systemd/system/nb-web-worker.service
mkdir -p "$root/data/web" "$root/data/worker" "$root/data/secrets"
if test ! -s "$root/data/secrets/device-secrets.json"; then
  test -s "$root/etc/device-secrets.json" || { echo "required private device secrets file is missing" >&2; exit 1; }
  install -m 0600 "$root/etc/device-secrets.json" "$root/data/secrets/device-secrets.json"
fi
chmod 0600 "$root/data/secrets/device-secrets.json"
chmod 0700 "$root/etc" "$root/data" "$root/data/web" "$root/data/worker" "$root/data/secrets"
systemctl daemon-reload
systemctl enable nb-web.service nb-web-worker.service
