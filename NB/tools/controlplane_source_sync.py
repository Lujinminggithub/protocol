#!/usr/bin/env python3
"""Atomically publish the tracked NB source snapshot to a Linux control plane."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import pathlib
import shlex
import tarfile
import nb_release

import paramiko


ROOT = pathlib.Path(__file__).resolve().parents[1]
ROLE_KEYS = {"entry": "edges", "middle": "relays", "exit": "terminals"}
ORCHESTRATION_FILES = (
    "tools/deploy.py",
    "tools/deploy_core.py",
    "tools/deploy_shard_runtime.py",
    "tools/deploy_transfer.py",
    "tools/nb_diag.py",
    "tools/nb_line_control.py",
    "tools/line_open.py",
    "tools/line_probe.py",
    "tools/line_provision.py",
    "tools/nb_release.py",
    "tools/node_release_upload.py",
    "tools/probe_cleanup.py",
    "tools/nb_shard_deploy.py",
    "tools/nb_observe.py",
    "tools/security_setup.py",
    "tools/whitelist_sync.py",
    "tools/worker_snapshot.py",
    "tools/test_deploy_transaction.py",
    "tools/test_deploy_transfer.py",
    "tools/test_controlplane_local_build.py",
    "tools/test_node_release_upload.py",
    "tools/test_diag_bundle.py",
    "tools/test_line_control.py",
    "tools/test_line_open.py",
    "tools/test_line_probe.py",
    "tools/test_line_provision.py",
    "tools/test_observe.py",
    "tools/test_probe_cleanup.py",
    "tools/test_release.py",
    "tools/test_shard_deploy.py",
    "tools/test_supervisor.py",
)


def source_manifest(files: dict[str, pathlib.Path]) -> dict[str, str]:
    missing = sorted(name for name, path in files.items() if not path.is_file())
    if missing:
        raise FileNotFoundError("source snapshot is incomplete: " + ", ".join(missing))
    return {
        name: hashlib.sha256(files[name].read_bytes()).hexdigest()
        for name in sorted(files)
    }


def deployment_source_files(root: pathlib.Path = ROOT) -> dict[str, pathlib.Path]:
    # Import lazily so source_manifest can be tested without loading a line inventory.
    import deploy

    files = dict(deploy.RELEASE_INPUTS)
    for name in ORCHESTRATION_FILES:
        files[name] = root.joinpath(*pathlib.PurePosixPath(name).parts)
    controlplane = root / "controlplane"
    for path in sorted(controlplane.rglob("*.go")):
        files[path.relative_to(root).as_posix()] = path
    for path in sorted((controlplane / "internal" / "webapp" / "assets").rglob("*")):
        if path.is_file():
            files[path.relative_to(root).as_posix()] = path
    for name in ("controlplane/go.mod", "controlplane/go.sum"):
        files[name] = root.joinpath(*pathlib.PurePosixPath(name).parts)
    return files


def validate_node_release(root: pathlib.Path = ROOT) -> dict:
    root = root.resolve()
    manifest = root / "build" / "release-manifest.json"
    binary = root / "build" / "nb_node"
    if not manifest.is_file() or not binary.is_file():
        raise RuntimeError("源码同步前必须存在匹配的 build/nb_node 和 build/release-manifest.json")
    git_info = nb_release.git_metadata(root)
    checked = nb_release.load_and_validate_manifest(manifest, root, binary, expected_git_commit=git_info["commit"])
    recorded = checked.get("git") or {}
    if recorded.get("commit") != git_info["commit"] or recorded.get("tree") != git_info["tree"]:
        raise RuntimeError("源码同步拒绝：Git commit/tree 与 node release manifest 不一致")
    return checked


def source_archive(files: dict[str, pathlib.Path]) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for name in sorted(files):
            archive.add(files[name], arcname=name, recursive=False)
    return buffer.getvalue()


def inventory_device(inventory: dict, role: str) -> dict:
    direct = inventory.get(role)
    if isinstance(direct, dict):
        return direct
    candidates = inventory.get(ROLE_KEYS[role], [])
    if isinstance(candidates, list) and len(candidates) == 1:
        return candidates[0]
    for candidate in inventory.get("machines", []):
        if candidate.get("role") == role:
            return candidate
    raise ValueError(f"inventory must contain exactly one {role} device")


def device_password(device: dict) -> str:
    env_name = str(device.get("password_env", "")).strip()
    password = os.environ.get(env_name, "") if env_name else ""
    password = password or str(device.get("password", ""))
    if not password:
        raise RuntimeError(f"{device.get('name', 'device')} is missing its SSH password reference")
    return password


def connect(device: dict, known_hosts: pathlib.Path) -> paramiko.SSHClient:
    client = paramiko.SSHClient()
    client.load_host_keys(str(known_hosts))
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    client.connect(
        hostname=device["host"], port=int(device.get("port", 22)),
        username=device.get("user", "root"), password=device_password(device),
        timeout=25, banner_timeout=25, auth_timeout=25,
        allow_agent=False, look_for_keys=False,
    )
    return client


def run(client: paramiko.SSHClient, command: str, timeout: int = 180) -> str:
    _stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
    output = stdout.read().decode("utf-8", "replace")
    error = stderr.read().decode("utf-8", "replace")
    status = stdout.channel.recv_exit_status()
    if status:
        raise RuntimeError((output + error).strip()[-4000:])
    return (output + error).strip()


def push_bytes(client: paramiko.SSHClient, data: bytes, target: str, mode: int = 0o600) -> None:
    digest = hashlib.sha256(data).hexdigest()
    partial = f"{target}.part.{digest[:16]}"
    run(client, f"install -d -m 0700 {shlex.quote(str(pathlib.PurePosixPath(target).parent))}")
    sftp = client.open_sftp()
    try:
        try:
            offset = sftp.stat(partial).st_size
        except OSError:
            offset = 0
        if offset > len(data):
            sftp.remove(partial)
            offset = 0
        with sftp.file(partial, "ab") as stream:
            for start in range(offset, len(data), 1024 * 1024):
                stream.write(data[start:start + 1024 * 1024])
            stream.flush()
        sftp.chmod(partial, mode)
    finally:
        sftp.close()
    actual = run(client, f"sha256sum {shlex.quote(partial)} | cut -d' ' -f1")
    if actual != digest:
        run(client, f"rm -f {shlex.quote(partial)}")
        raise RuntimeError("source archive upload checksum mismatch")
    run(client, f"mv -f {shlex.quote(partial)} {shlex.quote(target)}")


def publish(client: paramiko.SSHClient, files: dict[str, pathlib.Path], target: str) -> str:
    manifest = source_manifest(files)
    manifest_bytes = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode("ascii")
    snapshot = hashlib.sha256(manifest_bytes).hexdigest()[:16]
    parent = str(pathlib.PurePosixPath(target).parent)
    archive_path = f"{parent}/source-releases/{snapshot}.tar.gz"
    candidate = f"{target}.next.{snapshot}"
    backup = f"{parent}/source-releases/repo-before-{snapshot}"
    archive = source_archive(files)
    push_bytes(client, archive, archive_path)
    checks = "".join(f"{digest}  {name}\n" for name, digest in manifest.items()).encode("utf-8")
    checks_path = f"{parent}/source-releases/{snapshot}.sha256"
    push_bytes(client, checks, checks_path)
    prepare = f"""
set -e
rm -rf {shlex.quote(candidate)}
if test -d {shlex.quote(target)}; then cp -a {shlex.quote(target)} {shlex.quote(candidate)}; else mkdir -p {shlex.quote(candidate)}; fi
tar xzf {shlex.quote(archive_path)} -C {shlex.quote(candidate)}
cd {shlex.quote(candidate)}
sha256sum -c {shlex.quote(checks_path)} >/dev/null
"""
    run(client, prepare, timeout=600)
    activate = f"""
set -e
systemctl stop nb-web-worker.service
rm -rf {shlex.quote(backup)}
if test -d {shlex.quote(target)}; then mv {shlex.quote(target)} {shlex.quote(backup)}; fi
mv {shlex.quote(candidate)} {shlex.quote(target)}
if systemctl start nb-web-worker.service && systemctl is-active --quiet nb-web-worker.service; then
  echo SOURCE_SYNC_OK
else
  systemctl stop nb-web-worker.service 2>/dev/null || true
  mv {shlex.quote(target)} {shlex.quote(candidate)}
  if test -d {shlex.quote(backup)}; then mv {shlex.quote(backup)} {shlex.quote(target)}; fi
  systemctl start nb-web-worker.service 2>/dev/null || true
  exit 1
fi
"""
    if "SOURCE_SYNC_OK" not in run(client, activate, timeout=180):
        raise RuntimeError("worker did not confirm the published source snapshot")
    return snapshot


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("inventory", type=pathlib.Path)
    parser.add_argument("--known-hosts", required=True, type=pathlib.Path)
    parser.add_argument("--role", choices=tuple(ROLE_KEYS), default="middle")
    parser.add_argument("--target", default="/opt/nb-controlplane/repo")
    parser.add_argument("--allow-source-only", action="store_true",
                        help="仅同步源码，不校验 node release；仅允许本地开发使用")
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
    if not args.allow_source_only:
        release = validate_node_release()
        print(f"NODE_RELEASE_OK release={release['release_id']} git={release.get('git', {}).get('commit', '')}")
    files = deployment_source_files()
    manifest = source_manifest(files)
    client = connect(inventory_device(inventory, args.role), args.known_hosts)
    try:
        snapshot = publish(client, files, args.target)
    finally:
        client.close()
    total = sum(path.stat().st_size for path in files.values())
    print(f"SOURCE_SYNC_OK snapshot={snapshot} files={len(manifest)} bytes={total}")


if __name__ == "__main__":
    main()
