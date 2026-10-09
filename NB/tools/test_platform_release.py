#!/usr/bin/env python3
import json
import pathlib
import tempfile

import platform_release


with tempfile.TemporaryDirectory(prefix="platform-release-") as directory:
    root = pathlib.Path(directory)
    (root / "tools").mkdir()
    (root / "controlplane").mkdir()
    (root / "build").mkdir()
    (root / "tools" / "deploy.py").write_text("print('deploy')\n", encoding="utf-8")
    (root / "controlplane" / "nb-web").write_bytes(b"web-binary")
    (root / "controlplane" / "nb-web-worker").write_bytes(b"worker-binary")
    (root / "controlplane" / "nb-upgrader").write_bytes(b"upgrader-binary")
    (root / "build" / "nb_node").write_bytes(b"node-binary")
    node_manifest = {
        "release_id": "0123456789abcdef",
        "deployment_id": "0123456789abcdef-0123456789ab",
        "source_digest": "a" * 64,
        "node_version": {"product": "V200R001C01", "semantic": "2.1.1"},
        "artifact": {"sha256": platform_release.sha256_file(root / "build" / "nb_node")},
    }
    manifest = platform_release.create_platform_manifest(
        root,
        node_manifest,
        root / "controlplane" / "nb-web",
        root / "controlplane" / "nb-web-worker",
        root / "controlplane" / "nb-upgrader",
        script_paths=[root / "tools" / "deploy.py"],
        candidate_root=root,
    )
    assert manifest["schema_version"] == 1
    assert manifest["nb_node"]["sha256"] == node_manifest["artifact"]["sha256"]
    assert manifest["nb_web"]["sha256"] == platform_release.sha256_file(root / "controlplane" / "nb-web")
    assert manifest["nb_web_worker"]["sha256"] == platform_release.sha256_file(root / "controlplane" / "nb-web-worker")
    assert manifest["nb_upgrader"]["sha256"] == platform_release.sha256_file(root / "controlplane" / "nb-upgrader")
    assert manifest["scripts"]["files"][0]["path"] == "tools/deploy.py"
    assert manifest["candidate_root"] == str(root.resolve())
    encoded = json.dumps(manifest, sort_keys=True)
    assert manifest["release_id"] in encoded

try:
    platform_release.resolve_go({"NB_CONTROLPLANE_GO": str(pathlib.Path("missing-go"))})
    raise AssertionError("missing managed Go toolchain was accepted")
except RuntimeError as error:
    assert "Go" in str(error)

with tempfile.TemporaryDirectory(prefix="platform-release-cache-") as directory:
    base = pathlib.Path(directory)
    go = base / "go"
    go.write_text("test", encoding="utf-8")
    root = base / "repo"
    output = base / "output"
    (root / "controlplane").mkdir(parents=True)
    captured = []
    original_run = platform_release.subprocess.run
    try:
        def fake_run(command, **kwargs):
            captured.append(kwargs["env"])
            return type("Result", (), {"returncode": 0, "stdout": "", "stderr": ""})()
        platform_release.subprocess.run = fake_run
        platform_release.build_controlplane(root, output, {"NB_CONTROLPLANE_GO": str(go)})
    finally:
        platform_release.subprocess.run = original_run
    expected_cache = output / ".cache" / "go-build"
    assert captured and all(item["GOCACHE"] == str(expected_cache) for item in captured)
    assert expected_cache.is_dir()

print("platform release tests passed")
