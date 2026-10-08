#!/usr/bin/env python3
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import platform_upgrade


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

print("platform upgrade transaction tests passed")
