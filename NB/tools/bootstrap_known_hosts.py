#!/usr/bin/env python3
"""Explicit TOFU bootstrap for NB deployment host keys."""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
import pathlib
import tempfile


def host_token(host: dict) -> str:
    name = str(host["host"])
    port = int(host.get("port", 22))
    return name if port == 22 else f"[{name}]:{port}"


def fingerprint(key) -> str:
    digest = hashlib.sha256(key.asbytes()).digest()
    return "SHA256:" + base64.b64encode(digest).decode("ascii").rstrip("=")


def atomic_save(keys, destination: pathlib.Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".nb-known-hosts-", dir=destination.parent)
    os.close(fd)
    try:
        keys.save(temporary)
        os.chmod(temporary, 0o600)
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--accept-new", action="store_true")
    args = parser.parse_args()
    if not args.accept_new:
        raise SystemExit("--accept-new is required for explicit trust-on-first-use")
    if os.environ.get("NB_SSH_INSECURE") != "1":
        raise SystemExit("NB_SSH_INSECURE=1 is required only for this bootstrap command")

    import deploy_core as deploy

    keys = deploy.paramiko.HostKeys()
    for role in reversed(deploy.deployment_roles()):
        client = deploy.connect(role)
        try:
            key = client.get_transport().get_remote_server_key()
            keys.add(host_token(deploy.LAB[role]), key.get_name(), key)
            print(f"{role}: {key.get_name()} {fingerprint(key)}")
        finally:
            client.close()
    atomic_save(keys, args.output.resolve())
    print(f"saved {len(keys)} host keys to {args.output.resolve()}")


if __name__ == "__main__":
    main()
