#!/usr/bin/env python3
from __future__ import annotations

import copy
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import deploy_core  # noqa: E402
from line_open import baseline_profile, normalize_hosts  # noqa: E402


def main() -> None:
    source = {
        "topology_mode": "single_hk", "service_profile": "general",
        "entry": {"name": "hk-1", "host": "203.0.113.20", "port": 22,
                  "user": "root", "password": "test-only"},
        "exit": {"name": "hk-1", "host": "203.0.113.20", "port": 22,
                 "user": "root", "password": "test-only"},
    }
    hosts, credentials = normalize_hosts(source, exit_port=4450, topology_mode="single_hk")
    assert hosts["topology_mode"] == "single_hk" and hosts["service_profile"] == "general"
    assert "middle" not in hosts and set(hosts["workers"]) == {"entry", "exit"}
    assert hosts["exits"][0]["host"] == "127.0.0.1" and hosts["exits"][0]["port"] == 4450
    assert set(credentials) == {"NB_SSH_PASSWORD_ENTRY", "NB_SSH_PASSWORD_EXIT"}
    profile = baseline_profile(hosts, "hk-single", exit_port=4450)
    assert profile["active_path"] == ["hk-1", "hk-1"]
    assert profile["transport"]["entry_exit"]["address"] == "127.0.0.1:4450"
    assert profile["transport"]["fec"] == {
        "observe": False, "active": False, "codec": "off", "k": 0, "r": 0}

    saved_lab, saved_exit = copy.deepcopy(deploy_core.LAB), deploy_core.EXIT_PORT
    try:
        deploy_core.LAB = hosts
        deploy_core.EXIT_PORT = 4450
        assert deploy_core.deployment_roles() == ("exit", "entry")
        command = deploy_core._node_command("entry", socks_port=1082,
                                            wl_remote="/etc/NB/instances/hk/whitelist.conf")
        assert " -n 127.0.0.1 -N 4450 " in command and " -M ''" in command
        assert " -E " not in command and "middle" not in command.lower()
        environment = deploy_core._shard_instance_environment("entry", "cfg-test")
        assert environment["NB_FEC_V15"] == "off"
        assert environment["NB_FEC_V15_ACTIVE"] == "off"
    finally:
        deploy_core.LAB, deploy_core.EXIT_PORT = saved_lab, saved_exit
    print("single-HK plan tests passed")


if __name__ == "__main__":
    main()
