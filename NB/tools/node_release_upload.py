#!/usr/bin/env python3
"""Validate an uploaded Git repository, build node, and atomically activate its source tree."""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import tarfile
import tempfile
import time


def run(command: list[str], cwd: pathlib.Path) -> str:
    result = subprocess.run(command, cwd=cwd, check=False, capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).strip()[-4000:])
    return result.stdout.strip()


def safe_extract(archive_path: pathlib.Path, destination: pathlib.Path) -> pathlib.Path:
    with tarfile.open(archive_path, "r:gz") as archive:
        members = archive.getmembers()
        if len(members) > 200000:
            raise RuntimeError("源码包文件数量超过限制")
        total = sum(member.size for member in members if member.isfile())
        if total > 4 * 1024 * 1024 * 1024:
            raise RuntimeError("源码包解压后超过 4 GiB")
        for member in members:
            target = (destination / member.name).resolve()
            if destination.resolve() not in target.parents and target != destination.resolve():
                raise RuntimeError("源码包包含路径穿越")
            if member.issym() or member.islnk() or member.isdev():
                raise RuntimeError("源码包不允许链接或设备文件")
        archive.extractall(destination)
    roots = [item for item in destination.iterdir() if item.is_dir()]
    if (destination / ".git").is_dir():
        return destination
    if len(roots) == 1 and (roots[0] / ".git").is_dir():
        return roots[0]
    raise RuntimeError("上传内容必须是包含 .git 的完整 Git 仓库")


def project_root(repository: pathlib.Path) -> pathlib.Path:
    required = ("CMakeLists.txt", "src/nb_node.c", "controlplane/go.mod", "tools/deploy.py")
    if all((repository / path).is_file() for path in required):
        return repository
    candidates = [path for path in repository.iterdir() if path.is_dir() and
                  all((path / required_path).is_file() for required_path in required)]
    if len(candidates) != 1:
        raise RuntimeError("Git 仓库中必须且只能包含一个完整 NB 项目目录")
    return candidates[0]


def validate_git(root: pathlib.Path, expected_commit: str) -> dict:
    commit = run(["git", "rev-parse", "HEAD"], root)
    if commit != expected_commit:
        raise RuntimeError(f"Git commit 不匹配: expected={expected_commit} actual={commit}")
    dirty = run(["git", "status", "--porcelain", "--untracked-files=all"], root)
    if dirty:
        raise RuntimeError("上传的 Git 仓库不是 clean 工作区")
    tree = run(["git", "rev-parse", "HEAD^{tree}"], root)
    submodules = run(["git", "submodule", "status", "--recursive"], root)
    if submodules and any(line[:1] in {"+", "-", "U"} for line in submodules.splitlines()):
        raise RuntimeError("Git 子模块状态不一致")
    return {"commit": commit, "tree": tree, "submodules": submodules.splitlines() if submodules else []}


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
    parser.add_argument("--git-commit", required=True)
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
        git_info = validate_git(repository, args.git_commit)
        candidate = project_root(repository)
        git_info["project_path"] = candidate.relative_to(repository).as_posix()
        preserve_private_runtime(args.current_root, candidate)
        environment = os.environ.copy()
        environment["NB_FORCE_REMOTE_BUILD"] = "1"
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
        if recorded_git.get("commit") != git_info["commit"] or recorded_git.get("tree") != git_info["tree"]:
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
            "git_commit": git_info["commit"], "git_tree": git_info["tree"], "backup": str(backup),
        }, separators=(",", ":")))


if __name__ == "__main__":
    main()
