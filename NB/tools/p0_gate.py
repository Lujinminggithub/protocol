#!/usr/bin/env python3
"""本地 P0 发布门禁；Linux 构建机上的 C 测试由 deploy.py build 执行。"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys

from source_gate import enforce_source_line_limit, node_source_text


ROOT=pathlib.Path(__file__).resolve().parents[1]
TESTS=("test_release.py","test_deploy_transaction.py","test_observe.py","test_line_control.py",
       "test_diag_bundle.py","test_line_probe.py","test_supervisor.py","test_soak.py")
REQUIRED_MODULES=("nb_session.h","nb_session_index.c","nb_pool.h","nb_pool_health.c","nb_path.c","nb_dns.c","nb_bridge.c","nb_send.c","nb_metrics.c",
                  "nb_lstream.c","nb_lstream.h","nb_tenant_shared.c","nb_tenant_shared.h")


def main():
    for name in TESTS:subprocess.run([sys.executable,str(ROOT/"tools"/name)],cwd=ROOT,check=True)
    for name in REQUIRED_MODULES:
        if not (ROOT/"src"/name).is_file():raise RuntimeError(f"缺少 P0 模块: {name}")
    enforce_source_line_limit()
    source=node_source_text()
    if "picoquic_add_to_stream(" in source:raise RuntimeError("仍存在 add_to_stream 调用")
    json.loads((ROOT/"tools"/"lab-hosts.json").read_text(encoding="utf-8"))
    for path in ("controlplane/go.mod", "controlplane/cmd/nb-control/main.go",
                 "controlplane/nb-control.service", "monitoring/prometheus/nb-control.rules.yml",
                 "monitoring/grafana/nb-control.json", ".github/workflows/ci.yml",
                 "tools/deploy_controlplane.py", "tools/controlplane_config.py"):
        if not (ROOT/path).is_file():raise RuntimeError(f"missing P0 operations artifact: {path}")
    runtime_log=json.loads((ROOT/"tools"/"log4c.runtime.json").read_text(encoding="utf-8"))
    if runtime_log.get("log_dir")!="/etc/NB/logs":raise RuntimeError("P0 runtime log_dir 必须固定为 /etc/NB/logs")
    print("P0 GATE PASS")


if __name__=="__main__":main()
