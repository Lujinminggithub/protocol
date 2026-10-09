#!/usr/bin/env python3
import importlib.util
import pathlib
import sys
import types

ROOT = pathlib.Path(__file__).resolve().parents[1]
calls = []


class Connection:
    def close(self):
        calls.append(("close",))


fake_deploy = types.SimpleNamespace(
    WORK="/etc/NB",
    _effective_workers=lambda role: 2,
    _control_socket_path=lambda role, worker: f"/run/test-{role}-{worker}.ctl",
    connect=lambda role: (calls.append(("connect", role)) or Connection()),
    checked_run=lambda connection, command, tmo=0: (
        calls.append(("run", command)) or
        ('Newbility Node V200R001C03 (2.1.3)\n' if "--version" in command else
        ('{"status":"cleaned","before":1,"cleaned":1,"active":0}\n'
         if "probe cleanup" in command else
         '{"status":"ok","active":0}\n'))),
)
sys.modules["deploy"] = fake_deploy
sys.modules["nb_release"] = types.SimpleNamespace(
    NODE_PRODUCT_VERSION="V200R001C03", NODE_SEMANTIC_VERSION="2.1.3")
spec = importlib.util.spec_from_file_location("probe_cleanup", ROOT / "tools" / "probe_cleanup.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

result = module.cleanup_all()
assert [item["role"] for item in result] == ["entry", "entry", "middle", "middle", "exit", "exit"]
assert all(item["active"] == 0 for item in result)
assert sum("probe cleanup" in item[1] for item in calls if item[0] == "run") == 6
assert sum("probe status" in item[1] for item in calls if item[0] == "run") == 6
preflight = module.preflight_all()
assert len(preflight) == 3 and all(item["version"] == "V200R001C03 (2.1.3)" for item in preflight)

session_header = (ROOT / "src" / "nb_session.h").read_text(encoding="utf-8")
transport_source = (ROOT / "src" / "nb_node_transport.inc").read_text(encoding="utf-8")
control_source = (ROOT / "src" / "nb_node_core.inc").read_text(encoding="utf-8")
assert "int probe_traffic;" in session_header
assert "target_ok&&ps_internal_probe_target(thost,tport)" in transport_source
assert "G.streams[i].in_use&&G.streams[i].probe_traffic" in control_source
assert "picoquic_set_loss_reorder_tolerance(cnx,link->reorder_gap,link->reorder_delay_us)" in control_source
assert "if((G.role==ROLE_ENTRY||G.role==ROLE_MIDDLE)&&outbound&&" not in control_source
assert "transport generation transition pending" not in control_source

attempts = []
original_invoke = module.invoke


def partially_failing_invoke(connection, role, worker, command):
    attempts.append((role, worker, command))
    if role == "entry" and worker == 0 and command == "probe cleanup":
        raise RuntimeError("simulated cleanup failure")
    return {"cleaned": 1, "active": 0}


module.invoke = partially_failing_invoke
try:
    module.cleanup_all()
    raise AssertionError("partial cleanup failure was accepted")
except RuntimeError as exc:
    assert "entry[0] cleanup failed" in str(exc)
finally:
    module.invoke = original_invoke
assert ("exit", 1, "probe status") in attempts
print("probe_cleanup tests passed")
