from __future__ import annotations

import json
import pathlib
import tempfile

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
