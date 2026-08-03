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
