#!/usr/bin/env python3
"""Atomically install or update nb-control on the Entry host."""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shlex
import time

import deploy_core as deploy


ROOT = pathlib.Path(__file__).resolve().parents[1]
REMOTE_BINARY = "/usr/local/bin/nb-control"
REMOTE_ENV = "/etc/NB/nb-control.env"
REMOTE_UNIT = "/etc/systemd/system/nb-control.service"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def restore(client, backup: str) -> None:
    command = (
        f"if test -f {backup}/binary; then cp -p {backup}/binary {REMOTE_BINARY}; else rm -f {REMOTE_BINARY}; fi; "
        f"if test -f {backup}/environment; then cp -p {backup}/environment {REMOTE_ENV}; else rm -f {REMOTE_ENV}; fi; "
        f"if test -f {backup}/unit; then cp -p {backup}/unit {REMOTE_UNIT}; else rm -f {REMOTE_UNIT}; fi; "
        "systemctl daemon-reload; "
        "if test -f /etc/systemd/system/nb-control.service; then systemctl restart nb-control || true; "
        "else systemctl disable --now nb-control 2>/dev/null || true; fi"
    )
    deploy.run(client, command)


def deploy_control(binary: pathlib.Path, environment: pathlib.Path) -> None:
    binary_data = binary.read_bytes()
    environment_data = environment.read_bytes()
    unit_data = (ROOT / "controlplane" / "nb-control.service").read_bytes()
    if b"NB_CONTROL_API_TOKEN=" not in environment_data:
        raise RuntimeError("control-plane environment does not define NB_CONTROL_API_TOKEN")
    client = deploy.connect("entry")
    release = digest(binary_data)[:16]
    backup = f"/etc/NB/control-backups/{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}-{release}"
    locked = False
    try:
        deploy._acquire_deploy_lock(client, "entry-control", release)
        locked = True
        deploy.checked_run(client, f"mkdir -p {backup}; "
                   f"test ! -f {REMOTE_BINARY} || cp -p {REMOTE_BINARY} {backup}/binary; "
                   f"test ! -f {REMOTE_ENV} || cp -p {REMOTE_ENV} {backup}/environment; "
                   f"test ! -f {REMOTE_UNIT} || cp -p {REMOTE_UNIT} {backup}/unit")
        deploy.push_bytes(client, binary_data, REMOTE_BINARY + ".new", mode=0o755)
        deploy.push_bytes(client, environment_data, REMOTE_ENV + ".new", mode=0o600)
        deploy.push_bytes(client, unit_data, REMOTE_UNIT + ".new", mode=0o644)
        remote_hash = deploy.checked_run(client, f"sha256sum {shlex.quote(REMOTE_BINARY + '.new')} | cut -d' ' -f1").strip()
        if remote_hash != digest(binary_data):
            raise RuntimeError("remote control-plane binary hash mismatch")
        deploy.checked_run(client, f"mv -f {REMOTE_BINARY}.new {REMOTE_BINARY}; "
                   f"mv -f {REMOTE_ENV}.new {REMOTE_ENV}; mv -f {REMOTE_UNIT}.new {REMOTE_UNIT}; "
                   "systemctl daemon-reload; systemctl enable nb-control >/dev/null; systemctl restart nb-control")
        health = deploy.checked_run(client, "for i in $(seq 1 30); do "
                            "curl -fsS http://127.0.0.1:9080/readyz && exit 0; sleep 1; done; "
                            "systemctl status nb-control --no-pager -l; exit 1", tmo=45)
        state = deploy.checked_run(client, "systemctl is-active nb-control; "
                           "curl -fsS http://127.0.0.1:9080/metrics | head -8")
        print(f"nb-control release={release} backup={backup}")
        print(health.strip())
        print(state.strip())
    except Exception:
        restore(client, backup)
        raise
    finally:
        if locked:
            deploy._release_deploy_lock(client)
        client.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path, default=ROOT / "build" / "nb-control")
    parser.add_argument("--environment", type=pathlib.Path, required=True)
    args = parser.parse_args()
    deploy_control(args.binary.resolve(), args.environment.resolve())


if __name__ == "__main__":
    main()
