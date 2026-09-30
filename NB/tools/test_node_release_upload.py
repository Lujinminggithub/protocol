from __future__ import annotations

import json
import pathlib
import tarfile
import tempfile
import zipfile

import node_release_upload


with tempfile.TemporaryDirectory(prefix="node-release-upload-") as directory:
    base = pathlib.Path(directory)
    current = base / "repo"
    candidate = base / "candidate"
    (candidate / "tools").mkdir(parents=True)
    (base / "data" / "secrets").mkdir(parents=True)
    (candidate / "tools" / "lab-hosts.json").write_text(json.dumps({
        "entry": {"name": "entry-1", "password_env": "NB_SSH_PASSWORD_ENTRY"},
        "paths": {"work_dir": "/etc/NB"},
    }), encoding="utf-8")
    (base / "data" / "secrets" / "device-secrets.json").write_text(json.dumps({
        "device:entry-1": {"password": "controlled-secret"},
    }), encoding="utf-8")
    environment: dict[str, str] = {}
    node_release_upload.apply_build_credentials(current, candidate, environment)
    assert environment["NB_SSH_PASSWORD_ENTRY"] == "controlled-secret"

print("node release upload credential resolution passed")


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
