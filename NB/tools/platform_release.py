#!/usr/bin/env python3
"""Build and describe an immutable scripts/Web/Worker/Node platform candidate."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import pathlib
import subprocess
from collections.abc import Iterable, Mapping


SCHEMA_VERSION = 1
DEFAULT_GO = pathlib.Path("/opt/nb-controlplane/toolchains/go/bin/go")


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_go(environment: Mapping[str, str] | None = None) -> pathlib.Path:
    environment = os.environ if environment is None else environment
    candidate = pathlib.Path(environment.get("NB_CONTROLPLANE_GO", str(DEFAULT_GO)))
    if not candidate.is_file():
        raise RuntimeError(f"控制面缺少受管 Go 工具链: {candidate}")
    return candidate


def _artifact(path: pathlib.Path) -> dict:
    return {"path": path.name, "sha256": sha256_file(path), "size": path.stat().st_size}


def _scripts(root: pathlib.Path, paths: Iterable[pathlib.Path]) -> dict:
    records = []
    for path in sorted({item.resolve() for item in paths}):
        records.append({
            "path": path.relative_to(root.resolve()).as_posix(),
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        })
    digest = hashlib.sha256()
    for record in records:
        digest.update(f"{record['path']}\0{record['sha256']}\0{record['size']}\n".encode("utf-8"))
    return {"digest": digest.hexdigest(), "files": records}


def script_snapshot_paths(root: pathlib.Path) -> list[pathlib.Path]:
    result = []
    for relative in ("tools", "scripts", "controlplane/linux"):
        base = root / relative
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.is_file() and not any(part in {"private", "__pycache__"} for part in path.parts):
                result.append(path)
    return result


def build_controlplane(root: pathlib.Path, output: pathlib.Path,
                       environment: Mapping[str, str] | None = None) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    go = resolve_go(environment)
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ if environment is None else environment)
    env["CGO_ENABLED"] = "0"
    go_cache = output / ".cache" / "go-build"
    go_cache.mkdir(parents=True, exist_ok=True)
    env["GOCACHE"] = str(go_cache)
    commands = (
        [str(go), "test", "./cmd/...", "./internal/..."],
        [str(go), "build", "-trimpath", "-o", str(output / "nb-web"), "./cmd/nb-web"],
        [str(go), "build", "-trimpath", "-o", str(output / "nb-web-worker"), "./cmd/nb-web-worker"],
        [str(go), "build", "-trimpath", "-o", str(output / "nb-upgrader"), "./cmd/nb-upgrader"],
    )
    for command in commands:
        result = subprocess.run(command, cwd=root / "controlplane", env=env, check=False,
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).strip()[-8000:])
    return output / "nb-web", output / "nb-web-worker", output / "nb-upgrader"


def create_platform_manifest(root: pathlib.Path, node_manifest: dict, web: pathlib.Path,
                             worker: pathlib.Path, upgrader: pathlib.Path, *, script_paths: Iterable[pathlib.Path],
                             candidate_root: pathlib.Path) -> dict:
    scripts = _scripts(root, script_paths)
    node_artifact = node_manifest.get("artifact") or {}
    node = {
        "release_id": node_manifest["release_id"],
        "deployment_id": node_manifest["deployment_id"],
        "version": node_manifest["node_version"],
        "sha256": node_artifact["sha256"],
        "source_digest": node_manifest["source_digest"],
    }
    web_artifact, worker_artifact, upgrader_artifact = _artifact(web), _artifact(worker), _artifact(upgrader)
    identity = json.dumps({"scripts": scripts["digest"], "nb_web": web_artifact["sha256"],
                           "nb_web_worker": worker_artifact["sha256"], "nb_upgrader": upgrader_artifact["sha256"],
                           "nb_node": node["sha256"]},
                          sort_keys=True, separators=(",", ":")).encode("ascii")
    return {
        "schema_version": SCHEMA_VERSION,
        "release_id": hashlib.sha256(identity).hexdigest()[:16],
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "source_digest": node_manifest["source_digest"],
        "candidate_root": str(candidate_root.resolve()),
        "scripts": scripts,
        "nb_web": web_artifact,
        "nb_web_worker": worker_artifact,
        "nb_upgrader": upgrader_artifact,
        "nb_node": node,
    }


def write_manifest(path: pathlib.Path, manifest: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
