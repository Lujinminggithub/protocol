#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> None:
    code = r'''
import json,sys
sys.path.insert(0,"tools")
import deploy
middle=deploy._node_command("middle")
entry=deploy._node_command("entry",socks_port=1081,wl_remote=deploy._whitelist_remote())
print(json.dumps({
 "work":deploy.INSTANCE_WORK,
 "entry_service":deploy._service_name("entry"),
 "middle_socket":deploy._control_socket_path("middle",0),
 "log":deploy._log_path("exit"),
 "middle_command":middle,
 "entry_command":entry,
}))
'''
    env = os.environ.copy()
    env.update({"NB_DEPLOY_INSTANCE": "kz", "NB_SOCKS_PORT": "1081",
                "NB_MIDDLE_PORT": "4444", "NB_EXIT_PORT": "4443"})
    output = subprocess.check_output([sys.executable, "-c", code], cwd=ROOT, env=env, text=True)
    result = json.loads(output)
    assert result["work"] == "/etc/NB/instances/kz"
    assert result["entry_service"] == "nb-kz-entry"
    assert result["middle_socket"] == "/run/nb-kz-middle-0.ctl"
    assert result["log"] == "/etc/NB/instances/kz/logs/nb-kz-exit.log"
    assert " -p 4444" in result["middle_command"]
    assert " -l 1081 " in result["entry_command"] and " -N 4444 " in result["entry_command"]

    bad = env.copy(); bad["NB_DEPLOY_INSTANCE"] = "../bad"
    rejected = subprocess.run([sys.executable, "-c", code], cwd=ROOT, env=bad,
                              capture_output=True, text=True)
    assert rejected.returncode != 0
    print("deploy namespace tests passed")


if __name__ == "__main__":
    main()
