#!/usr/bin/env python3
"""NB 构建产物清单生成与完整性校验。"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import pathlib
import re
import subprocess
from typing import Iterable, Mapping


SCHEMA_VERSION = 2
NODE_PRODUCT_VERSION = "V200R001C00"
NODE_SEMANTIC_VERSION = "2.1.0"
RELEASE_NAME_RE = re.compile(r"^(?:[0-9a-f]{16}(?:-[0-9a-f]{12})?|legacy-[0-9a-f]{16})$")
DEPLOYMENT_NAME_RE = re.compile(
    r"^(?:(?:[0-9a-f]{16}(?:-[0-9a-f]{12})?|legacy-[0-9a-f]{16})|cfg-[0-9a-f]{16})$")


def is_binary_input(path: str) -> bool:
    """Return whether a release input can change the nb_node binary."""
    normalized = str(path).replace("\\", "/")
    if normalized in {"VERSION", "CMakeLists.txt", "third_party/picoquic/build_libs.sh"}:
        return True
    return pathlib.PurePosixPath(normalized).suffix in {".c", ".h", ".inc", ".a"}


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _file_record(root: pathlib.Path, path: pathlib.Path, logical_path: str | None = None) -> dict:
    resolved = path.resolve()
    if logical_path is None:
        try:
            logical_path = resolved.relative_to(root.resolve()).as_posix()
        except ValueError:
            logical_path = resolved.name
    return {
        "path": logical_path,
        "sha256": sha256_file(resolved),
        "size": resolved.stat().st_size,
    }


def _source_digest(records: Iterable[dict]) -> str:
    digest = hashlib.sha256()
    for record in sorted(records, key=lambda item: item["path"]):
        digest.update(f"{record['path']}\0{record['sha256']}\0{record['size']}\n".encode("utf-8"))
    return digest.hexdigest()


def git_metadata(root: pathlib.Path, expected_commit: str | None = None, require_clean: bool = True) -> dict:
    root = root.resolve()
    def git(*args: str) -> str:
        result = subprocess.run(["git", "-C", str(root), *args], check=False,
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise ValueError(f"Git 操作失败: {' '.join(args)}: {detail}")
        return result.stdout.strip()

    top = pathlib.Path(git("rev-parse", "--show-toplevel")).resolve()
    try:
        project_path = root.relative_to(top).as_posix()
    except ValueError as error:
        raise ValueError(f"源码目录不在 Git 工作树内: {root}") from error
    commit = git("rev-parse", "HEAD")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Git HEAD 不是完整 commit")
    if expected_commit and commit != expected_commit.lower():
        raise ValueError(f"Git commit 不匹配: expected={expected_commit} actual={commit}")
    dirty = git("status", "--porcelain", "--untracked-files=all")
    if dirty and require_clean:
        preview = "\\n".join(dirty.splitlines()[:20])
        raise ValueError(f"Git 工作区不是 clean，拒绝构建:\n{preview}")
    submodules = git("submodule", "status", "--recursive")
    if submodules and any(line[:1] in {"+", "-", "U"} for line in submodules.splitlines()):
        raise ValueError("Git 子模块未固定在提交记录指定版本")
    tree = git("rev-parse", "HEAD^{tree}")
    return {"commit": commit, "tree": tree, "project_path": project_path, "dirty": bool(dirty),
            "changes": dirty.splitlines()[:100] if dirty else [],
            "submodules": submodules.splitlines() if submodules else []}


def snapshot_inputs(root: pathlib.Path, inputs: Mapping[str, pathlib.Path]) -> list[dict]:
    return [
        _file_record(root.resolve(), path, logical_path)
        for logical_path, path in sorted(inputs.items())
        if path.is_file()
    ]


def select_release_removals(entries: Iterable[tuple[str, float]], protected: set[str], retain: int) -> list[str]:
    if not 2 <= retain <= 20:
        raise ValueError("发布保留数量必须为 2..20")
    valid = [(name, modified) for name, modified in entries if RELEASE_NAME_RE.fullmatch(name)]
    ordered = [name for name, _ in sorted(valid, key=lambda item: item[1], reverse=True)]
    keep = set(ordered[:retain]) | {name for name in protected if RELEASE_NAME_RE.fullmatch(name)}
    return [name for name in ordered if name not in keep]


def create_manifest(
    root: pathlib.Path,
    binary: pathlib.Path,
    platform: str,
    inputs: Mapping[str, pathlib.Path],
    topology: pathlib.Path,
    line_profile: pathlib.Path | None = None,
    configuration_inputs: Mapping[str, pathlib.Path] | None = None,
    git_info: dict | None = None,
) -> dict:
    root = root.resolve()
    artifact = _file_record(root, binary)
    input_records = snapshot_inputs(root, inputs)
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "release_id": artifact["sha256"][:16],
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "platform": platform,
        "node_version": {"product": NODE_PRODUCT_VERSION, "semantic": NODE_SEMANTIC_VERSION},
        "artifact": artifact,
        "source_digest": _source_digest(input_records),
        "inputs": input_records,
        "topology": _file_record(root, topology),
    }
    if git_info and git_info.get("commit") and git_info.get("tree"):
        manifest["git"] = {
            "commit": git_info["commit"],
            "tree": git_info["tree"],
            "project_path": git_info.get("project_path", "."),
            "dirty": bool(git_info.get("dirty")),
            "submodules": git_info.get("submodules", []),
        }
    if line_profile is not None and line_profile.is_file():
        manifest["line_profile"] = _file_record(root, line_profile)
    runtime_configuration = snapshot_inputs(root, configuration_inputs or {})
    manifest["runtime_configuration"] = runtime_configuration
    config_records = [manifest["topology"]] + ([manifest["line_profile"]] if "line_profile" in manifest else []) + runtime_configuration
    config_digest = _source_digest(config_records)
    manifest["configuration_digest"] = config_digest
    manifest["deployment_id"] = f"{manifest['release_id']}-{config_digest[:12]}"
    return manifest


def write_manifest(path: pathlib.Path, manifest: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def load_and_validate_manifest(
    manifest_path: pathlib.Path,
    root: pathlib.Path,
    binary: pathlib.Path,
    topology: pathlib.Path | None = None,
    line_profile: pathlib.Path | None = None,
    expected_git_commit: str | None = None,
) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"不支持的发布清单版本: {manifest.get('schema_version')}")
    if manifest.get("node_version") != {"product": NODE_PRODUCT_VERSION, "semantic": NODE_SEMANTIC_VERSION}:
        raise ValueError("Node 版本信息无效或不受支持")
    artifact = manifest.get("artifact") or {}
    actual_hash = sha256_file(binary)
    actual_size = binary.stat().st_size
    if artifact.get("sha256") != actual_hash or artifact.get("size") != actual_size:
        raise ValueError("发布清单与 nb_node 二进制不一致")
    if manifest.get("release_id") != actual_hash[:16]:
        raise ValueError("发布编号与二进制哈希不一致")
    git_info = manifest.get("git") or {}
    if git_info:
        current_git = git_metadata(root, expected_git_commit, require_clean=False)
        if git_info.get("commit") != current_git["commit"] or git_info.get("tree") != current_git["tree"]:
            raise ValueError("Git commit/tree 与发布清单不一致")

    records = manifest.get("inputs")
    if not isinstance(records, list) or not records:
        raise ValueError("发布清单缺少构建输入")
    binary_records = [record for record in records if is_binary_input(record.get("path", ""))]
    if not binary_records:
        raise ValueError("发布清单缺少 Node 二进制构建输入")
    for record in binary_records:
        source = root / record["path"]
        if not source.is_file():
            raise ValueError(f"构建输入已缺失: {record['path']}")
        if sha256_file(source) != record.get("sha256") or source.stat().st_size != record.get("size"):
            raise ValueError(f"构建输入已变化，请重新构建: {record['path']}")
    # Older manifests also recorded deployment Python/config files. Keep them
    # readable during migration, but never let those files force a Node
    # rebuild or invalidate an otherwise matching binary.
    if all(is_binary_input(record.get("path", "")) for record in records):
        if manifest.get("source_digest") != _source_digest(records):
            raise ValueError("构建输入摘要无效")
    configuration_paths = {"topology": topology, "line_profile": line_profile}
    for key in ("topology", "line_profile"):
        record = manifest.get(key)
        if not record:
            continue
        current = configuration_paths[key] or (root / record["path"])
        if not current.is_file() or sha256_file(current) != record.get("sha256") or current.stat().st_size != record.get("size"):
            raise ValueError(f"{key} 已变化，请重新生成发布清单")
    runtime_configuration = manifest.get("runtime_configuration")
    if not isinstance(runtime_configuration, list):
        raise ValueError("release manifest is missing runtime configuration")
    for record in runtime_configuration:
        current = root / record["path"]
        if not current.is_file() or sha256_file(current) != record.get("sha256") or current.stat().st_size != record.get("size"):
            raise ValueError(f"runtime configuration changed; rebuild required: {record['path']}")
    config_records = [manifest["topology"]] + ([manifest["line_profile"]] if manifest.get("line_profile") else []) + runtime_configuration
    config_digest = _source_digest(config_records)
    if manifest.get("configuration_digest") != config_digest or manifest.get("deployment_id") != f"{actual_hash[:16]}-{config_digest[:12]}":
        raise ValueError("部署配置摘要无效")
    return manifest
