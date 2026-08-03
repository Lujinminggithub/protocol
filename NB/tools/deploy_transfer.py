#!/usr/bin/env python3
"""Resumable node-to-node release transfer helpers."""
from __future__ import annotations

import json
import os
import pathlib
import shlex


def _transfer_candidates(host: dict) -> list[str]:
    values = [host.get("private_ip"), host.get("jump_target_host"), host.get("host")]
    result = []
    for value in values:
        value = str(value or "").strip()
        if value and value not in result:
            result.append(value)
    if not result:
        raise ValueError("target has no transfer address")
    return result


def copy_release(source_c, source_role, target_c, target_role, manifest, *,
                 instance_work, role_host, run, push_bytes):
    """Copy an immutable release over node-to-node SSH using a temporary key."""
    release_id = manifest.get("deployment_id") or manifest["release_id"]
    expected = manifest["artifact"]["sha256"]
    size = int(manifest["artifact"]["size"])
    source = f"{instance_work}/releases/{release_id}/nb_node"
    destination = source
    existing = run(target_c,
        f"test -f {shlex.quote(destination)} && sha256sum {shlex.quote(destination)} | awk '{{print $1}}' || true").strip()
    if existing == expected:
        print(f"{source_role} -> {target_role}: release={release_id} already present and verified")
        return "already-present"
    if existing:
        raise RuntimeError(f"{target_role} immutable release collision: {release_id}")

    target = role_host(target_role)
    user = str(target.get("user") or "root").strip()
    port = int(target.get("port", 22))
    if not user or any(ch not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-" for ch in user):
        raise ValueError(f"{target_role} has an invalid SSH user")
    if not 1 <= port <= 65535:
        raise ValueError(f"{target_role} has an invalid SSH port")

    marker = f"nb-release-{release_id}-{source_role}-to-{target_role}"
    key_path = f"/tmp/{marker}"
    known_hosts = f"{key_path}.known_hosts"
    addresses = _transfer_candidates(target)
    server_key = target_c.get_transport().get_remote_server_key()
    key_record = f"{server_key.get_name()} {server_key.get_base64()}"
    host_lines = []
    for address in addresses:
        host_token = address if port == 22 else f"[{address}]:{port}"
        host_lines.append(f"{host_token} {key_record}")

    timeout = max(60, min(3600, int(os.environ.get("NB_REMOTE_TRANSFER_TIMEOUT_SECONDS", "900"))))
    authorized = None
    try:
        dependencies = run(source_c,
            "for x in ssh ssh-keygen tail sha256sum; do command -v $x >/dev/null 2>&1 || echo MISSING:$x; done")
        if "MISSING:" in dependencies:
            missing = ", ".join(line.split(":", 1)[1] for line in dependencies.splitlines()
                                if line.startswith("MISSING:"))
            raise RuntimeError(f"{source_role} is missing transfer dependencies: {missing}")
        run(source_c,
            f"rm -f {shlex.quote(key_path)} {shlex.quote(key_path + '.pub')} {shlex.quote(known_hosts)}; "
            f"ssh-keygen -q -t ed25519 -N '' -C {shlex.quote(marker)} -f {shlex.quote(key_path)}; "
            f"chmod 0600 {shlex.quote(key_path)}")
        public_key = run(source_c, f"cat {shlex.quote(key_path + '.pub')}").strip().split()
        if len(public_key) < 2:
            raise RuntimeError("ephemeral transfer key generation failed")
        authorized = f"{public_key[0]} {public_key[1]} {marker}"
        run(target_c,
            "mkdir -p \"$HOME/.ssh\"; chmod 0700 \"$HOME/.ssh\"; "
            f"printf '\\n%s\\n' {shlex.quote(authorized)} >>\"$HOME/.ssh/authorized_keys\"; "
            "chmod 0600 \"$HOME/.ssh/authorized_keys\"")
        push_bytes(source_c, ("\n".join(host_lines) + "\n").encode("ascii"), known_hosts, mode=0o600)

        failures = []
        for address in addresses:
            endpoint = f"{user}@{address}"
            ssh = (f"ssh -i {shlex.quote(key_path)} -o BatchMode=yes -o IdentitiesOnly=yes "
                   f"-o StrictHostKeyChecking=yes -o UserKnownHostsFile={shlex.quote(known_hosts)} "
                   f"-o ConnectTimeout=10 -p {port} {shlex.quote(endpoint)}")
            part = f"{destination}.part.{expected[:16]}"
            try:
                parent = str(pathlib.PurePosixPath(destination).parent)
                run(source_c, f"{ssh} {shlex.quote('mkdir -p ' + shlex.quote(parent))}", tmo=30)
                size_cmd = f"test -f {shlex.quote(part)} && stat -c %s {shlex.quote(part)} || echo 0"
                offset = int(run(source_c, f"{ssh} {shlex.quote(size_cmd)}", tmo=30).strip().splitlines()[-1])
                if offset > size:
                    run(source_c, f"{ssh} {shlex.quote('rm -f ' + shlex.quote(part))}", tmo=30)
                    offset = 0
                if offset < size:
                    append = shlex.quote("cat >> " + shlex.quote(part))
                    run(source_c, f"tail -c +{offset + 1} {shlex.quote(source)} | {ssh} {append}", tmo=timeout)
                publish = (f"test \"$(sha256sum {shlex.quote(part)} | awk '{{print $1}}')\" = "
                           f"{shlex.quote(expected)} && chmod 0755 {shlex.quote(part)} && "
                           f"mv -f {shlex.quote(part)} {shlex.quote(destination)} && echo VERIFIED")
                result = run(source_c, f"{ssh} {shlex.quote(publish)}", tmo=60)
                if "VERIFIED" not in result:
                    raise RuntimeError("remote checksum verification failed")
                print(f"{source_role} -> {target_role}: release={release_id} transferred via {address}:{port}")
                return address
            except Exception as error:
                failures.append(f"{address}:{port}: {error}")
        raise RuntimeError(f"{source_role} -> {target_role} transfer failed: {'; '.join(failures)}")
    finally:
        try:
            run(source_c, f"rm -f {shlex.quote(key_path)} {shlex.quote(key_path + '.pub')} {shlex.quote(known_hosts)}")
        except Exception:
            pass
        if authorized is not None:
            try:
                cleanup = ("auth=\"$HOME/.ssh/authorized_keys\"; tmp=\"$auth.nb-clean.$$\"; "
                           f"awk '$NF != {json.dumps(marker)}' \"$auth\" >\"$tmp\" || true; "
                           "cat \"$tmp\" >\"$auth\"; rm -f \"$tmp\"")
                run(target_c, cleanup)
            except Exception:
                pass
