#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import deploy_media_reserve_shared as rollout  # noqa: E402


events: list[tuple[str, str]] = []
old_digest = "ab" * 32
state = {
    "unit": "/etc/systemd/system/nb-entry-shard@.service",
    "unit_backup": "/backup/entry/unit.service",
    "previous": "/etc/NB/shards/releases/c57f28878d62f943/nb_node",
}


def checked_run(_client, command: str, **_kwargs) -> str:
    events.append(("run", command))
    if command.startswith("systemctl is-active"):
        return "active" if any("systemctl start" in value for kind, value in events if kind == "run") else "inactive"
    if command.startswith("systemctl show"):
        return "1234"
    if command.startswith("sha256sum /proc/") or command.startswith("sha256sum /etc/NB/shards/releases/"):
        return old_digest + "  binary"
    return ""


rollout.deploy.checked_run = checked_run
rollout.deploy.push_bytes = lambda _client, _data, path, **_kwargs: events.append(("push", path))
rollout.deploy.fetch_bytes = lambda _client, _path: b"old-unit"
rollout.deploy_shard_runtime.verify_all_controls = (
    lambda *_args, **_kwargs: events.append(("verify", "controls"))
)
rollout.controls_health = lambda *_args, **_kwargs: [{"status": "ok"}, {"status": "ok"}]
rollout.time.sleep = lambda _seconds: None

result = rollout.rollback_group(object(), "entry", state)
commands = [value for kind, value in events if kind == "run"]
stop_at = next(index for index, value in enumerate(commands) if value.startswith("systemctl stop"))
link_at = next(index for index, value in enumerate(commands) if "ln -sfn" in value)
start_at = next(index for index, value in enumerate(commands) if value.startswith("systemctl start"))
assert stop_at < link_at < start_at, commands
assert len([value for value in commands if value.startswith("systemctl is-active")]) == 4
assert result == {"release_id": "c57f28878d62f943", "controls": 2,
                  "hashes": [old_digest, old_digest]}
print("media reserve deployment rollback test passed")
