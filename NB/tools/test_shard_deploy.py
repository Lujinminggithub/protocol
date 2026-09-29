#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import os

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import nb_shard_deploy as shard
import deploy_shard_runtime as runtime


def main() -> None:
    command = "/etc/xgw/instances/line-a/nb_node -r entry -l 1082 -n 10.0.0.2 -N 4444 -S"
    rendered = shard.render_instance_config("line-a", "entry", 1, command,
        {"NB_CC": "bbr", "NB_LINE_PROFILE_ID": "line-a"}, 256, 32 * 1024 * 1024)
    assert "instance_id=line-a-entry" in rendered
    assert "control_path=/run/nb-line-a-entry-1.ctl" in rendered
    assert "arg=--max-sessions\narg=256" in rendered
    assert "env.NB_WORKER_ID=1" in rendered
    assert "env.NB_RELEASE_ID" not in rendered
    unit = shard.render_systemd_unit("/etc/xgw", "middle", "abc-def", 512, 768)
    assert "nb_node --shard-dir /etc/xgw/shards/configs/middle/%i" in unit
    assert "MemoryHigh=512M" in unit and "MemoryMax=768M" in unit
    assert "NB_BINARY_RELEASE_ID=abc-def" in unit and "NB_RELEASE_ID=" not in unit
    assert shard.saved_instance_config("/etc/xgw/instances/line-a", "deploy-1", "entry", 1).endswith(
        "/releases/deploy-1/shard-entry-1.conf")
    assert shard.saved_binary_release("/etc/xgw/instances/line-a", "deploy-1", "entry").endswith(
        "/releases/deploy-1/shard-entry-binary-release")
    assert runtime.active_sessions(None, "entry", work="/etc/xgw",
        run=lambda *_args, **_kwargs: "3\n") == 3
    previous_drain = os.environ.get("NB_SHARD_DRAIN_SECONDS")
    os.environ["NB_SHARD_DRAIN_SECONDS"] = "0"
    try:
        runtime.require_idle_for_binary_change(None, "entry", work="/etc/xgw",
            run=lambda *_args, **_kwargs: "1\n")
    except RuntimeError as error:
        assert "1 active sessions" in str(error)
    else:
        raise AssertionError("binary rollout accepted an active line")
    finally:
        if previous_drain is None:
            os.environ.pop("NB_SHARD_DRAIN_SECONDS", None)
        else:
            os.environ["NB_SHARD_DRAIN_SECONDS"] = previous_drain

    commands = []
    pushed = []
    started_workers = set()

    def cold_start_run(_client, command, **_kwargs):
        commands.append(command)
        if "responses.append" in command:
            raise AssertionError("dangling shared binary link triggered a session drain query")
        if "echo STAGED" in command:
            return "STAGED\n"
        if command.startswith("readlink -f "):
            return "/etc/xgw/shards/releases/old/nb_node\n"
        if command.startswith("if test -x "):
            return "ABSENT\n"
        if "SHARD_CONTROLS_OK" in command:
            if len(started_workers) != 2:
                raise AssertionError("all controls were checked before both cold-start workers ran")
            return "SHARD_CONTROLS_OK 2\n"
        if command.startswith("systemctl is-active "):
            return "inactive\n"
        if command.startswith("systemctl enable "):
            for worker in (0, 1):
                if f"nb-middle-shard@{worker}" in command:
                    started_workers.add(worker)
            return "active\n"
        if command.startswith("if test -f "):
            return "ABSENT\n"
        return ""

    result = runtime.install_role(
        None, "middle", command, {}, "deploy-new", "new", 0,
        work="/etc/xgw", instance_work="/etc/xgw/instances/line-a",
        deploy_instance="line-a", lab={}, run=cold_start_run,
        push_bytes=lambda _client, data, path, **kwargs: pushed.append((path, data, kwargs)),
        effective_workers=lambda _role: 2,
        legacy_service_name=lambda role: f"nb-{role}.service")
    assert "binary_changed=true" in result
    assert started_workers == {0, 1}
    assert any(path == "/etc/systemd/system/nb-middle-shard@.service" for path, _, _ in pushed)
    assert not any("responses.append" in item for item in commands)

    for invalid in ("bad id", "../escape", ""):
        try:
            shard.render_instance_config(invalid, "entry", 0, command, {}, 1, 1024 * 1024)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe instance id accepted")
    print("RESULT PASS")


if __name__ == "__main__":
    main()
