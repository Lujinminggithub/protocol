#!/usr/bin/env python3
"""Generate private nb-control API credentials and environment."""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import secrets


def private_write(path: pathlib.Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(data, encoding="utf-8")
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=pathlib.Path, required=True)
    parser.add_argument("--line-id", required=True)
    parser.add_argument("--instance-id", default="")
    parser.add_argument("--web-base-url", default="")
    parser.add_argument("--web-token", default="")
    parser.add_argument("--web-signing-key", default="")
    args = parser.parse_args()
    token = secrets.token_urlsafe(32)
    if args.instance_id and (len(args.instance_id) > 48 or not all(c.isalnum() or c in "._-" for c in args.instance_id)):
        raise SystemExit("invalid --instance-id")
    work = f"/etc/NB/instances/{args.instance_id}" if args.instance_id else "/etc/NB"
    socket_prefix = f"nb-{args.instance_id}-entry" if args.instance_id else "nb-entry"
    values = {
        "NB_CONTROL_LISTEN": "127.0.0.1:9080",
        "NB_CONTROL_STATE_DIR": "/var/lib/nb-control",
        "NB_CONTROL_USERS_FILE": f"{work}/socks.users",
        "NB_CONTROL_TENANTS_FILE": f"{work}/tenant.conf",
        "NB_CONTROL_SOCKET_GLOB": f"/run/{socket_prefix}-*.ctl",
        "NB_LINE_ID": args.line_id,
        "NB_CONTROL_API_TOKEN": token,
        "NB_WEB_BASE_URL": args.web_base_url,
        "NB_WEB_TOKEN": args.web_token,
        "NB_WEB_SIGNING_KEY": args.web_signing_key,
    }
    directory = args.directory.resolve()
    private_write(directory / "nb-control.env", "".join(f"{key}={json.dumps(value)}\n" for key, value in values.items()))
    private_write(directory / "nb-control-client.json", json.dumps(
        {"base_url": "http://127.0.0.1:9080", "api_token": token}, indent=2) + "\n")
    print(f"generated private control-plane configuration in {directory}")


if __name__ == "__main__":
    main()
