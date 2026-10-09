#!/usr/bin/env python3
import json
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import platform_upgrade

repo_root = pathlib.Path(platform_upgrade.__file__).resolve().parents[1]
for unit in ("nb-web.service", "nb-web-worker.service", "nb-upgrader.service"):
    assert "TimeoutStopSec=500ms" in (repo_root / "controlplane" / "linux" / unit).read_text(encoding="utf-8")
upgrader_unit = (repo_root / "controlplane" / "linux" / "nb-upgrader.service").read_text(encoding="utf-8")
assert "ReadWritePaths=/opt/nb-controlplane /etc/systemd/system" in upgrader_unit
worker_unit = (repo_root / "controlplane" / "linux" / "nb-web-worker.service").read_text(encoding="utf-8")
assert "Requires=nb-web.service" not in worker_unit
assert "Wants=network-online.target nb-web.service" in worker_unit


class FakeAdapter:
    def __init__(self, fail_at=None):
        self.calls = []
        self.fail_at = fail_at

    def switch_scripts(self):
        self.calls.append(("scripts",))

    def rollback_scripts(self):
        self.calls.append(("rollback-scripts",))

    def switch_controlplane(self, service):
        self.calls.append(("controlplane", service))

    def rollback_controlplane(self, service):
        self.calls.append(("rollback-controlplane", service))

    def stage_node(self, role):
        self.calls.append(("stage", role))

    def activate_node(self, role, worker, deadline_seconds):
        self.calls.append(("activate", role, worker, deadline_seconds))
        if (role, worker) == self.fail_at:
            raise RuntimeError("injected activation failure")
        return {"sessions_interrupted": 7, "downtime_ms": 900}

    def rollback_node(self, role, worker, deadline_seconds):
        self.calls.append(("rollback", role, worker, deadline_seconds))

    def smoke(self):
        self.calls.append(("smoke",))


adapter = FakeAdapter()
result = platform_upgrade.execute_transaction(adapter, workers=2, deadline_seconds=2.0)
assert adapter.calls[:3] == [("scripts",), ("controlplane", "nb-web-worker"), ("controlplane", "nb-web")]
assert [call for call in adapter.calls if call[0] == "stage"] == [
    ("stage", "entry"), ("stage", "middle"), ("stage", "exit")]
assert [call[:3] for call in adapter.calls if call[0] == "activate"] == [
    ("activate", "exit", 0), ("activate", "exit", 1),
    ("activate", "middle", 0), ("activate", "middle", 1),
    ("activate", "entry", 0), ("activate", "entry", 1)]
assert result["sessions_interrupted"] == 42
assert adapter.calls[-1] == ("smoke",)

failed = FakeAdapter(fail_at=("middle", 1))
try:
    platform_upgrade.execute_transaction(failed, workers=2, deadline_seconds=2.0)
    raise AssertionError("failed activation was accepted")
except RuntimeError as error:
    assert "injected activation failure" in str(error)
rollbacks = [call[:3] for call in failed.calls if call[0] == "rollback"]
assert rollbacks == [
    ("rollback", "middle", 1),
    ("rollback", "middle", 0),
    ("rollback", "exit", 1),
    ("rollback", "exit", 0),
]
assert [(call[0], call[1]) for call in failed.calls if call[0] == "rollback-controlplane"] == [
    ("rollback-controlplane", "nb-web"),
    ("rollback-controlplane", "nb-web-worker"),
]
assert failed.calls[-1] == ("rollback-scripts",)

manual = FakeAdapter()
platform_upgrade.execute_rollback(manual, workers=2, deadline_seconds=2.0)
assert [call[:3] for call in manual.calls if call[0] == "rollback"] == [
    ("rollback", "entry", 1), ("rollback", "entry", 0),
    ("rollback", "middle", 1), ("rollback", "middle", 0),
    ("rollback", "exit", 1), ("rollback", "exit", 0),
]
assert manual.calls[-3:] == [
    ("rollback-controlplane", "nb-web"),
    ("rollback-controlplane", "nb-web-worker"),
    ("rollback-scripts",),
]


class RollbackFailAdapter(FakeAdapter):
    def rollback_node(self, role, worker, deadline_seconds):
        super().rollback_node(role, worker, deadline_seconds)
        if (role, worker) == ("entry", 1):
            raise RuntimeError("entry unavailable")


partial = RollbackFailAdapter()
try:
    platform_upgrade.execute_rollback(partial, workers=2, deadline_seconds=2.0)
    raise AssertionError("partial rollback was accepted")
except RuntimeError as error:
    assert "entry unavailable" in str(error)
assert ("rollback", "exit", 0, 2.0) in partial.calls
assert partial.calls[-1] == ("rollback-scripts",)

with tempfile.TemporaryDirectory() as directory:
    install_root = pathlib.Path(directory)
    current = install_root / "repo"
    candidate = install_root / "source-candidates" / "ready-test"
    (candidate / "build").mkdir(parents=True)
    current.mkdir()
    (current / "identity.txt").write_text("current", encoding="utf-8")
    (candidate / "identity.txt").write_text("candidate", encoding="utf-8")
    manifest_path = candidate / "build" / "platform-release.json"
    manifest_path.write_text(json.dumps({
        "release_id": "release-reusable",
        "candidate_root": str(candidate),
        "scripts": {"files": []},
    }), encoding="utf-8")
    previous_root = os.environ.get("NB_CONTROLPLANE_ROOT")
    os.environ["NB_CONTROLPLANE_ROOT"] = str(current)
    try:
        system = platform_upgrade.SystemAdapter(manifest_path, "line-a", "op-first")
        system.switch_scripts()
        assert candidate.is_dir(), "platform candidate was consumed by first upgrade unit"
        assert (current / "identity.txt").read_text(encoding="utf-8") == "candidate"
        system.rollback_scripts()
        assert candidate.is_dir(), "platform candidate disappeared after rollback"
        assert (current / "identity.txt").read_text(encoding="utf-8") == "current"
    finally:
        if previous_root is None:
            os.environ.pop("NB_CONTROLPLANE_ROOT", None)
        else:
            os.environ["NB_CONTROLPLANE_ROOT"] = previous_root

print("platform upgrade transaction tests passed")
