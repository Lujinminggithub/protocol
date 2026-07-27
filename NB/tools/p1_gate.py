#!/usr/bin/env python3
"""P1 结构与控制面回归门禁；Linux CTest 由 deploy build 继续执行。"""
from __future__ import annotations
import json,pathlib,subprocess,sys
from source_gate import enforce_source_line_limit,node_source_text
ROOT=pathlib.Path(__file__).resolve().parents[1]
def run(name:str)->None:subprocess.run([sys.executable,str(ROOT/"tools"/name)],check=True,cwd=ROOT)
def main()->None:
    for name in ("test_deploy_connection.py","test_observe.py","test_supervisor.py","test_p1_control.py","test_security_rotate.py",
                 "test_line_provision.py","test_line_open.py","test_netem_matrix.py"):
        run(name)
    required=("src/nb_tenant.c","src/nb_tenant.h","src/nb_tenant_shared.c","src/nb_tenant_shared.h","tools/test_tenant.c","src/nb_whitelist.c","src/nb_whitelist.h","tools/test_whitelist.c","src/nb_udp_io.c","src/nb_udp_io.h","tools/test_udp_io.c","src/nb_udp_lifecycle.c","src/nb_udp_lifecycle.h","tools/test_udp_lifecycle.c","tools/nb_p1_control.py","tools/security_rotate.py")
    for name in required:
        if not (ROOT/name).is_file():raise RuntimeError(f"missing P1 module: {name}")
    enforce_source_line_limit()
    node=node_source_text()
    deploy="\n".join((ROOT/path).read_text(encoding="utf-8") for path in ("tools/deploy.py","tools/deploy_core.py"))
    runtri=(ROOT/"scripts/runtri.sh").read_text(encoding="utf-8");lab=json.loads((ROOT/"tools/lab-hosts.json").read_text(encoding="utf-8"))
    for token in ("client_udp_fd","nb_routes_pick_key","nb_tenant_acquire","worker_lane_port"):
        if token not in node:raise RuntimeError(f"P1 node integration missing: {token}")
    tenant=(ROOT/"src/nb_tenant.c").read_text(encoding="utf-8")
    if "nb_tenant_shared_open" not in tenant or "nb_tenant_shared_allowance" not in tenant:
        raise RuntimeError("P1 shared tenant state is not integrated")
    if "nb_routes_finish_session(&G.exit_routes" not in node or "nb_routes_report_result(&G.exit_routes" in node:
        raise RuntimeError("business sessions must not drive route health")
    if "middle worker 强制降为 1" in deploy:raise RuntimeError("middle multi-worker is still disabled")
    for token in ('"src/nb_tenant.c"','"tools/test_tenant.c"','"tools/nb_supervisor.py"'):
        if token not in deploy:raise RuntimeError(f"remote build input missing: {token}")
    if any(token in runtri for token in ("127.0.0.1:8080","127.0.0.1:9000")):raise RuntimeError("runtri still uses shared fixed test ports")
    for token in ("short business sessions keep route health neutral","UDP child close synchronized across entry/middle/exit"):
        if token not in runtri:raise RuntimeError(f"P1 integration regression missing: {token}")
    if lab.get("workers")!={"entry":2,"middle":2,"exit":2}:raise RuntimeError("P1 worker topology must be entry=2 middle=2 exit=2")
    if "NB_ENTRY_WORKERS={_effective_workers('entry')}" not in deploy or "NB_MIDDLE_WORKERS={_effective_workers('middle')}" not in deploy or "NB_EXIT_WORKERS={_effective_workers('exit')}" not in deploy:raise RuntimeError("remote build does not exercise configured worker counts")
    print("P1 GATE PASS")
if __name__=="__main__":main()
