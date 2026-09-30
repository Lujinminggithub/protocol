#!/usr/bin/env python3
"""Validate an uploaded Git repository, build node, and atomically activate its source tree."""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time
import zipfile


def run(command: list[str], cwd: pathlib.Path) -> str:
    result = subprocess.run(command, cwd=cwd, check=False, capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).strip()[-4000:])
    return result.stdout.strip()


def safe_extract(archive_path: pathlib.Path, destination: pathlib.Path) -> pathlib.Path:
    def validate_target(name: str) -> None:
        target = (destination / name).resolve()
        if destination.resolve() not in target.parents and target != destination.resolve():
            raise RuntimeError("源码包包含路径穿越")

    if zipfile.is_zipfile(archive_path):
        with zipfile.ZipFile(archive_path) as archive:
            members = archive.infolist()
            if len(members) > 200000:
                raise RuntimeError("源码包文件数量超过限制")
            if sum(member.file_size for member in members) > 4 * 1024 * 1024 * 1024:
                raise RuntimeError("源码包解压后超过 4 GiB")
            for member in members:
                validate_target(member.filename)
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise RuntimeError("源码包不允许符号链接")
            archive.extractall(destination)
    else:
        try:
            archive = tarfile.open(archive_path, "r:*")
        except tarfile.TarError as error:
            raise RuntimeError("源码包必须是 ZIP、TAR 或 TAR.GZ") from error
        with archive:
            members = archive.getmembers()
            if len(members) > 200000:
                raise RuntimeError("源码包文件数量超过限制")
            total = sum(member.size for member in members if member.isfile())
            if total > 4 * 1024 * 1024 * 1024:
                raise RuntimeError("源码包解压后超过 4 GiB")
            for member in members:
                validate_target(member.name)
                if member.issym() or member.islnk() or member.isdev():
                    raise RuntimeError("源码包不允许链接或设备文件")
            archive.extractall(destination)
    roots = [item for item in destination.iterdir() if item.is_dir()]
    if (destination / ".git").is_dir():
        return destination
    if len(roots) == 1:
        return roots[0]
    return destination


def project_root(repository: pathlib.Path) -> pathlib.Path:
    required = ("CMakeLists.txt", "src/nb_node.c", "controlplane/go.mod", "tools/deploy.py")
    if all((repository / path).is_file() for path in required):
        return repository
    candidates = [path for path in repository.iterdir() if path.is_dir() and
                  all((path / required_path).is_file() for required_path in required)]
    if len(candidates) != 1:
        raise RuntimeError("Git 仓库中必须且只能包含一个完整 NB 项目目录")
    return candidates[0]


def collect_git_metadata(root: pathlib.Path) -> dict:
    if not (root / ".git").exists():
        return {"commit": "", "tree": "", "dirty": True, "changes": [], "submodules": []}
    commit = run(["git", "rev-parse", "HEAD"], root)
    tree = run(["git", "rev-parse", "HEAD^{tree}"], root)
    dirty = run(["git", "status", "--porcelain", "--untracked-files=all"], root)
    submodules = run(["git", "submodule", "status", "--recursive"], root)
    return {"commit": commit, "tree": tree, "dirty": bool(dirty), "changes": dirty.splitlines()[:100] if dirty else [],
            "submodules": submodules.splitlines() if submodules else []}


def preserve_private_runtime(current: pathlib.Path, candidate: pathlib.Path) -> None:
    for relative in ("tools/private", "build/security", "build/line-profiles"):
        source, target = current / relative, candidate / relative
        if source.exists() and not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, target)


def apply_build_credentials(current_root: pathlib.Path, candidate: pathlib.Path, environment: dict[str, str]) -> None:
    inventory_path = pathlib.Path(environment.get("NB_HOSTS_FILE", str(candidate / "tools" / "lab-hosts.json")))
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    build_role = str(inventory.get("build_host") or "entry")
    role_keys = {"entry": "edges", "middle": "relays", "exit": "terminals"}
    device = inventory.get(build_role)
    if not isinstance(device, dict):
        candidates = inventory.get(role_keys[build_role]) or []
        if len(candidates) != 1:
            raise RuntimeError(f"构建拓扑必须且只能包含一个 {build_role} 设备")
        device = candidates[0]
    password_env = str(device.get("password_env") or f"NB_SSH_PASSWORD_{build_role.upper()}")
    if environment.get(password_env):
        return
    secrets_path = current_root.parent / "data" / "secrets" / "device-secrets.json"
    secrets = json.loads(secrets_path.read_text(encoding="utf-8"))
    device_id = str(device.get("id") or device.get("name") or "")
    secret = secrets.get("device:" + device_id) or secrets.get(device_id) or {}
    password = str(secret.get("password") or "") if isinstance(secret, dict) else ""
    if not password:
        raise RuntimeError(f"构建机 {device_id} 缺少受控设备凭据")
    environment[password_env] = password


def activate(current: pathlib.Path, candidate: pathlib.Path, operation_id: str) -> pathlib.Path:
    backup = current.with_name(f"repo-before-{operation_id}-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}")
    os.replace(current, backup)
    try:
        os.replace(candidate, current)
    except Exception:
        os.replace(backup, current)
        raise
    return backup


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True, type=pathlib.Path)
    parser.add_argument("--current-root", required=True, type=pathlib.Path)
    parser.add_argument("--operation-id", required=True)
    args = parser.parse_args()
    if not args.archive.is_file():
        raise RuntimeError("上传源码包不存在")
    staging_parent = args.current_root.parent / "source-candidates"
    staging_parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=args.operation_id + "-", dir=staging_parent) as temporary:
        extracted = pathlib.Path(temporary) / "extract"
        extracted.mkdir()
        repository = safe_extract(args.archive, extracted)
        git_info = collect_git_metadata(repository)
        candidate = project_root(repository)
        git_info["project_path"] = candidate.relative_to(repository).as_posix()
        preserve_private_runtime(args.current_root, candidate)
        environment = os.environ.copy()
        environment["NB_FORCE_REMOTE_BUILD"] = "1"
        environment["NB_ALLOW_UNVERSIONED_SOURCE"] = "1"
        inventory = candidate / "tools" / "private" / "kz-machines.json"
        known_hosts = candidate / "tools" / "private" / "kz-known_hosts"
        if inventory.is_file():
            environment["NB_HOSTS_FILE"] = str(inventory)
        if known_hosts.is_file():
            environment["NB_KNOWN_HOSTS"] = str(known_hosts)
        else:
            installed_known_hosts = args.current_root.parent / "etc" / "known_hosts"
            if installed_known_hosts.is_file():
                environment["NB_KNOWN_HOSTS"] = str(installed_known_hosts)
        if not environment.get("NB_KNOWN_HOSTS"):
            raise RuntimeError("Node Release 构建缺少受信任的 known_hosts")
        apply_build_credentials(args.current_root, candidate, environment)
        result = subprocess.run([environment.get("PYTHON", "python3"), "tools/deploy.py", "build"], cwd=candidate,
                                env=environment, check=False, capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).strip()[-8000:])
        manifest_path = candidate / "build" / "release-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        recorded_git = manifest.get("git") or {}
        if recorded_git and (recorded_git.get("commit") != git_info["commit"] or recorded_git.get("tree") != git_info["tree"]):
            raise RuntimeError("构建产物 Git 信息与上传仓库不一致")
        (candidate / ".nb-git.json").write_text(json.dumps(git_info, separators=(",", ":")) + "\n", encoding="utf-8")
        final_candidate = staging_parent / f"ready-{args.operation_id}"
        if final_candidate.exists():
            shutil.rmtree(final_candidate)
        os.replace(candidate, final_candidate)
        backup = activate(args.current_root, final_candidate, args.operation_id)
        print("NODE_RELEASE_JSON=" + json.dumps({
            "release_id": manifest["release_id"], "deployment_id": manifest["deployment_id"],
            "binary_sha256": manifest["artifact"]["sha256"], "source_digest": manifest["source_digest"],
            "node_version": manifest["node_version"], "git_commit": git_info["commit"], "git_tree": git_info["tree"],
            "git_dirty": git_info["dirty"], "backup": str(backup),
        }, separators=(",", ":")))


if __name__ == "__main__":
    main()
