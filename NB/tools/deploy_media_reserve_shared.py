#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shlex
import sys
import time

import deploy_core as deploy
import deploy_shard_runtime
import nb_shard_deploy


ROOT = pathlib.Path(__file__).resolve().parents[1]
ROLES = ("exit", "middle", "entry")
EXPECTED_PREVIOUS = "c57f28878d62f943"
UNIT_TEMPLATE = "/etc/systemd/system/nb-{role}-shard@.service"


def services(role: str) -> list[str]:
    return [nb_shard_deploy.shard_service(role, worker)
            for worker in range(deploy._effective_workers(role))]


def controls_health(client, role: str, release_id: str) -> list[dict]:
    root = nb_shard_deploy.shard_root(deploy.WORK)
    script = (
        "import json,pathlib,socket;items=[];"
        f"root=pathlib.Path({str(root + '/configs/' + role)!r});"
        "configs=sorted(root.glob('*/*.conf'));"
        "paths=[next(x.split('=',1)[1] for x in p.read_text().splitlines() "
        "if x.startswith('control_path=')) for p in configs];"
        "\nfor path in paths:"
        "\n s=socket.socket(socket.AF_UNIX);s.settimeout(3);s.connect(path);"
        "s.sendall(b'health\\n');items.append(json.loads(s.recv(16384)));s.close()"
        "\nprint(json.dumps({'expected':len(paths),'items':items},separators=(',',':')))"
    )
    raw = deploy.checked_run(client, f"python3 -c {shlex.quote(script)}", tmo=30)
    result = json.loads(raw.strip().splitlines()[-1])
    items = result.get("items")
    expected = result.get("expected")
    if (type(expected) is not int or expected <= 0 or not isinstance(items, list) or
            len(items) != expected or any(item.get("status") != "ok" or
            item.get("binary_release_id") != release_id for item in items)):
        raise RuntimeError(f"{role} shared control health mismatch")
    return items


def stage(client, role: str, binary: bytes, release_id: str, digest: str) -> dict:
    sessions = deploy_shard_runtime.active_sessions(client, role, work=deploy.WORK, run=deploy.run)
    if sessions and not os.environ.get("NB_FORCE_ACTIVE_DEPLOY"):
        raise RuntimeError(f"{role} has {sessions} active sessions")
    if sessions:
        print(f"{role}: forcing deployment with {sessions} active sessions")

    root = f"{deploy.WORK}/shards"
    target = f"{root}/releases/{release_id}/nb_node"
    deploy.push_bytes(client, binary, target, mode=0o755)
    actual = deploy.checked_run(client, f"sha256sum {shlex.quote(target)}", tmo=20).split()[0]
    if actual != digest:
        raise RuntimeError(f"{role} staged binary hash mismatch")

    unit = UNIT_TEMPLATE.format(role=role)
    previous = deploy.checked_run(client, f"readlink -f {shlex.quote(root + '/nb_node')}", tmo=15).strip()
    unit_bytes = deploy.fetch_bytes(client, unit)
    marker = f"Environment=NB_BINARY_RELEASE_ID={release_id}".encode()
    lines = unit_bytes.splitlines()
    next_lines = [marker if line.startswith(b"Environment=NB_BINARY_RELEASE_ID=") else line for line in lines]
    if sum(line.startswith(b"Environment=NB_BINARY_RELEASE_ID=") for line in lines) != 1:
        raise RuntimeError(f"{role} unit binary release marker is invalid")
    next_unit = b"\n".join(next_lines) + b"\n"

    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    backup_root = f"{root}/deploy-backups/{release_id}-{stamp}/{role}"
    deploy.checked_run(client, f"mkdir -p {shlex.quote(backup_root)}", tmo=15)
    deploy.checked_run(client, f"chmod 0700 {shlex.quote(backup_root)}", tmo=15)
    deploy.push_bytes(client, unit_bytes, backup_root + "/unit.service", mode=0o644)
    return {"role": role, "previous": previous, "unit": unit,
            "unit_backup": backup_root + "/unit.service", "next_unit": next_unit}


def activate_group(client, role: str, state: dict, release_id: str, digest: str) -> dict:
    units = services(role)
    deploy.checked_run(client, "systemctl stop " + " ".join(map(shlex.quote, units)), tmo=60)
    for unit in units:
        if deploy.checked_run(client,
                f"systemctl is-active {shlex.quote(unit)} 2>/dev/null || true", tmo=15).strip() == "active":
            raise RuntimeError(f"{unit} did not stop for shared-state migration")

    deploy.push_bytes(client, state["next_unit"], state["unit"], mode=0o644)
    root = f"{deploy.WORK}/shards"
    link = root + "/nb_node"
    target = f"releases/{release_id}/nb_node"
    deploy.checked_run(client, f"ln -sfn {shlex.quote(target)} {shlex.quote(link + '.next')}", tmo=15)
    deploy.checked_run(client, f"mv -Tf {shlex.quote(link + '.next')} {shlex.quote(link)}", tmo=15)
    deploy.checked_run(client, "systemctl daemon-reload", tmo=30)
    deploy.checked_run(client, "systemctl start " + " ".join(map(shlex.quote, units)), tmo=60)
    time.sleep(5)

    for unit in units:
        if deploy.checked_run(client, f"systemctl is-active {shlex.quote(unit)}", tmo=15).strip() != "active":
            raise RuntimeError(f"{unit} is not active")
    deploy_shard_runtime.verify_all_controls(client, role, work=deploy.WORK, run=deploy.run)
    controls = controls_health(client, role, release_id)
    hashes = []
    for unit in units:
        pid = deploy.checked_run(client,
            f"systemctl show -p MainPID --value {shlex.quote(unit)}", tmo=15).strip()
        actual = deploy.checked_run(client, f"sha256sum /proc/{pid}/exe", tmo=15).split()[0]
        if actual != digest:
            raise RuntimeError(f"{unit} running binary hash mismatch")
        hashes.append(actual)
    return {"controls": len(controls), "hashes": hashes}


def rollback_group(client, role: str, state: dict) -> dict:
    units = services(role)
    previous = pathlib.PurePosixPath(state["previous"])
    root = pathlib.PurePosixPath(deploy.WORK) / "shards"
    release_id = previous.parent.name
    digest = deploy.checked_run(client, f"sha256sum {shlex.quote(str(previous))}", tmo=15).split()[0]

    deploy.checked_run(client, "systemctl stop " + " ".join(map(shlex.quote, units)), tmo=60)
    for unit in units:
        if deploy.checked_run(client,
                f"systemctl is-active {shlex.quote(unit)} 2>/dev/null || true", tmo=15).strip() == "active":
            raise RuntimeError(f"{unit} did not stop for shared-state rollback")

    deploy.push_bytes(client, deploy.fetch_bytes(client, state["unit_backup"]), state["unit"], mode=0o644)
    relative = previous.relative_to(root)
    link = str(root / "nb_node")
    deploy.checked_run(client,
        f"ln -sfn {shlex.quote(str(relative))} {shlex.quote(link + '.rollback')}", tmo=15)
    deploy.checked_run(client,
        f"mv -Tf {shlex.quote(link + '.rollback')} {shlex.quote(link)}", tmo=15)
    deploy.checked_run(client, "systemctl daemon-reload", tmo=30)
    deploy.checked_run(client, "systemctl start " + " ".join(map(shlex.quote, units)), tmo=60)
    time.sleep(5)

    for unit in units:
        if deploy.checked_run(client, f"systemctl is-active {shlex.quote(unit)}", tmo=15).strip() != "active":
            raise RuntimeError(f"{unit} is not active after rollback")
    deploy_shard_runtime.verify_all_controls(client, role, work=deploy.WORK, run=deploy.run)
    controls = controls_health(client, role, release_id)
    hashes = []
    for unit in units:
        pid = deploy.checked_run(client,
            f"systemctl show -p MainPID --value {shlex.quote(unit)}", tmo=15).strip()
        actual = deploy.checked_run(client, f"sha256sum /proc/{pid}/exe", tmo=15).split()[0]
        if actual != digest:
            raise RuntimeError(f"{unit} rollback binary hash mismatch")
        hashes.append(actual)
    return {"release_id": release_id, "controls": len(controls), "hashes": hashes}


def main() -> None:
    binary = (ROOT / "build" / "nb_node").read_bytes()
    manifest = json.loads((ROOT / "build" / "release-manifest.json").read_text(encoding="utf-8"))
    digest = hashlib.sha256(binary).hexdigest()
    release_id = digest[:16]
    if manifest.get("release_id") != release_id or manifest.get("artifact", {}).get("sha256") != digest:
        raise RuntimeError("candidate binary does not match release manifest")

    os.environ["NB_FORCE_ACTIVE_DEPLOY"] = "1"
    clients = {}
    states = {}
    activated = []
    try:
        for role in ROLES:
            clients[role] = deploy.connect(role)
            states[role] = stage(clients[role], role, binary, release_id, digest)
            previous = pathlib.PurePosixPath(states[role]["previous"]).parent.name
            if previous != EXPECTED_PREVIOUS:
                raise RuntimeError(f"{role} current shared release is {previous}, expected {EXPECTED_PREVIOUS}")

        results = {}
        for role in ROLES:
            activated.append(role)
            results[role] = activate_group(clients[role], role, states[role], release_id, digest)
        print(json.dumps({"status": "deployed", "release_id": release_id,
            "sha256": digest, "previous": EXPECTED_PREVIOUS, "roles": results},
            ensure_ascii=False, separators=(",", ":")))
    except Exception:
        for role in reversed(activated):
            try:
                rollback_group(clients[role], role, states[role])
            except Exception as error:
                print(f"回滚失败 role={role} error={error}", file=sys.stderr)
        raise
    finally:
        for client in clients.values():
            client.close()


if __name__ == "__main__":
    main()
