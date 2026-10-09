from __future__ import annotations

import json
import pathlib
import subprocess
import tarfile
import tempfile
import zipfile

import node_release_upload


with tempfile.TemporaryDirectory(prefix="node-release-controlplane-build-") as directory:
    base = pathlib.Path(directory)
    current = base / "repo"
    candidate = base / "candidate"
    (candidate / "tools").mkdir(parents=True)
    (base / "etc").mkdir(parents=True)
    (candidate / "tools" / "lab-hosts.json").write_text(json.dumps({
        "entry": {"name": "entry-1", "password_env": "NB_SSH_PASSWORD_ENTRY"},
        "paths": {"work_dir": "/etc/NB"},
    }), encoding="utf-8")
    (base / "etc" / "known_hosts").write_text("entry ssh-ed25519 AAAATEST\n", encoding="utf-8")
    environment = node_release_upload.controlplane_build_environment(current, candidate, {})
    assert environment["NB_KNOWN_HOSTS"] == str(base / "etc" / "known_hosts")
    assert environment["NB_CONTROLPLANE_GO_CACHE_ROOT"] == str(base / "data" / "go-build-cache")
    assert "NB_SSH_PASSWORD_ENTRY" not in environment

print("node release control-plane build isolation passed")


def add_repository_files(write_file) -> None:
    write_file("repo/.git/HEAD", b"ref: refs/heads/main\n")
    write_file("repo/CMakeLists.txt", b"project(nb)\n")


with tempfile.TemporaryDirectory(prefix="node-release-archives-") as directory:
    base = pathlib.Path(directory)
    archives = []
    for name, mode in (("source.tar", "w"), ("source.tar.gz", "w:gz")):
        path = base / name
        with tarfile.open(path, mode) as archive:
            def write_tar(filename: str, data: bytes) -> None:
                info = tarfile.TarInfo(filename)
                info.size = len(data)
                import io
                archive.addfile(info, io.BytesIO(data))
            add_repository_files(write_tar)
        archives.append(path)
    zip_path = base / "source.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        add_repository_files(archive.writestr)
    archives.append(zip_path)
    for archive_path in archives:
        destination = base / (archive_path.name + "-extract")
        destination.mkdir()
        repository = node_release_upload.safe_extract(archive_path, destination)
        assert (repository / ".git" / "HEAD").is_file(), archive_path

print("node release archive formats passed")


with tempfile.TemporaryDirectory(prefix="node-release-dirty-") as directory:
    repository = pathlib.Path(directory)
    subprocess.run(["git", "init"], cwd=repository, check=True, capture_output=True)
    (repository / "tracked.txt").write_text("tracked\n", encoding="utf-8")
    subprocess.run(["git", "add", "."], cwd=repository, check=True)
    subprocess.run(["git", "-c", "user.email=test@example.com", "-c", "user.name=test", "commit", "-m", "initial"],
                   cwd=repository, check=True, capture_output=True)
    (repository / "uploaded-change.txt").write_text("build this content\n", encoding="utf-8")
    metadata = node_release_upload.collect_git_metadata(repository)
    assert metadata["dirty"] is True
    assert any("uploaded-change.txt" in line for line in metadata["changes"])

print("dirty Git upload metadata accepted")


with tempfile.TemporaryDirectory(prefix="node-release-overlay-") as directory:
    base = pathlib.Path(directory)
    current, candidate = base / "current", base / "candidate"
    for repository in (current, candidate):
        (repository / "tools").mkdir(parents=True)
    (current / "tools" / "deploy.py").write_text("trusted\n", encoding="utf-8")
    (candidate / "tools" / "deploy.py").write_text("uploaded\n", encoding="utf-8")
    node_release_upload.overlay_controlplane_orchestration(current, candidate)
    assert (candidate / "tools" / "deploy.py").read_text(encoding="utf-8") == "trusted\n"

print("node release orchestration overlay passed")


with tempfile.TemporaryDirectory(prefix="node-release-ready-") as directory:
    base = pathlib.Path(directory)
    current = base / "repo"
    candidate = base / "candidate"
    current.mkdir()
    candidate.mkdir()
    (current / "identity.txt").write_text("current\n", encoding="utf-8")
    (candidate / "identity.txt").write_text("candidate\n", encoding="utf-8")
    ready = node_release_upload.stage_candidate(current, candidate, "op-test")
    assert (current / "identity.txt").read_text(encoding="utf-8") == "current\n"
    assert ready == base / "source-candidates" / "ready-op-test"
    assert (ready / "identity.txt").read_text(encoding="utf-8") == "candidate\n"

print("node release candidate remains inactive until platform upgrade")
