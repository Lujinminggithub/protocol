#!/usr/bin/env python3
"""Render the fixed NB shard runtime and per-line instance configurations."""
from __future__ import annotations

import pathlib
import re
import shlex


SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,48}$")
SAFE_ENV = re.compile(r"^[A-Z_][A-Z0-9_]{0,62}$")
ROLES = ("entry", "middle", "exit")


def shard_root(work: str) -> str:
    return f"{work}/shards"


def shard_service(role: str, worker: int) -> str:
    if role not in ROLES or worker < 0 or worker > 31:
        raise ValueError("invalid shard identity")
    return f"nb-{role}-shard@{worker}"


def shard_config_dir(work: str, role: str, worker: int) -> str:
    shard_service(role, worker)
    return f"{shard_root(work)}/configs/{role}/{worker}"


def line_control_path(instance: str, role: str, worker: int) -> str:
    if not SAFE_ID.fullmatch(instance) or role not in ROLES or worker < 0 or worker > 31:
        raise ValueError("invalid line control identity")
    value = f"/run/nb-{instance}-{role}-{worker}.ctl"
    if len(value.encode("utf-8")) >= 108:
        raise ValueError("line control path exceeds sockaddr_un")
    return value


def saved_instance_config(instance_work: str, release_id: str, role: str, worker: int) -> str:
    if (not SAFE_ID.fullmatch(release_id) or role not in ROLES or worker < 0 or worker > 31):
        raise ValueError("invalid saved shard config identity")
    return f"{instance_work}/releases/{release_id}/shard-{role}-{worker}.conf"


def saved_binary_release(instance_work: str, release_id: str, role: str) -> str:
    if not SAFE_ID.fullmatch(release_id) or role not in ROLES:
        raise ValueError("invalid saved shard release identity")
    return f"{instance_work}/releases/{release_id}/shard-{role}-binary-release"


def current_deployment_marker(instance_work: str) -> str:
    return f"{instance_work}/current-deployment"


def saved_previous_instance_config(instance_work: str, release_id: str,
                                   role: str, worker: int) -> str:
    if not SAFE_ID.fullmatch(release_id) or role not in ROLES or worker < 0 or worker > 31:
        raise ValueError("invalid previous shard config identity")
    return f"{instance_work}/releases/{release_id}/previous-shard-{role}-{worker}.conf"


def saved_previous_instance_state(instance_work: str, release_id: str,
                                  role: str, worker: int) -> str:
    return saved_previous_instance_config(instance_work, release_id, role, worker) + ".state"


def saved_previous_deployment_marker(instance_work: str, release_id: str) -> str:
    if not SAFE_ID.fullmatch(release_id):
        raise ValueError("invalid previous deployment marker identity")
    return f"{instance_work}/releases/{release_id}/previous-current-deployment"


def command_arguments(command: str) -> list[str]:
    values = shlex.split(command)
    if len(values) < 3 or pathlib.PurePosixPath(values[0]).name != "nb_node":
        raise ValueError("instance command must execute nb_node")
    return values[1:]


def render_instance_config(instance: str, role: str, worker: int, command: str,
                           environment: dict[str, str], max_sessions: int,
                           max_queue_bytes: int) -> str:
    if not 1 <= max_sessions <= 1024:
        raise ValueError("max_sessions must be 1..1024")
    if not 1024 * 1024 <= max_queue_bytes <= 1024 * 1024 * 1024:
        raise ValueError("max_queue_bytes must be 1 MiB..1 GiB")
    runtime_instance=f"{instance}-{role}"
    if len(runtime_instance)>64:
        raise ValueError("runtime instance id exceeds 64 characters")
    arguments = command_arguments(command)
    arguments += ["--max-sessions", str(max_sessions),
                  "--max-queue-bytes", str(max_queue_bytes)]
    lines = ["schema=1", f"instance_id={runtime_instance}",
             f"control_path={line_control_path(instance, role, worker)}"]
    for value in arguments:
        if not value or any(character in value for character in "\r\n\0"):
            raise ValueError("unsafe shard argument")
        lines.append(f"arg={value}")
    merged = dict(environment)
    merged.update({"NB_INSTANCE_ID": instance, "NB_WORKER_ID": str(worker),
                   "NB_WORKER_LANE_PORTS": "on",
                   "NB_CONTROL_PREFIX": f"nb-{instance}-{role}"})
    for name in sorted(merged):
        value = str(merged[name])
        if not SAFE_ENV.fullmatch(name) or any(character in value for character in "\r\n\0"):
            raise ValueError("unsafe shard environment")
        lines.append(f"env.{name}={value}")
    return "\n".join(lines) + "\n"


def render_systemd_unit(work: str, role: str, binary_release_id: str,
                        memory_high_mb: int, memory_max_mb: int) -> str:
    if role not in ROLES or not SAFE_ID.fullmatch(binary_release_id):
        raise ValueError("invalid shard unit identity")
    if memory_high_mb < 128 or memory_max_mb < memory_high_mb:
        raise ValueError("invalid shard memory limits")
    root = shard_root(work)
    return f"""[Unit]
Description=Newbility {role} shard worker %i
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
Environment=MALLOC_ARENA_MAX=2
Environment=NB_BINARY_RELEASE_ID={binary_release_id}
ExecStart={root}/nb_node --shard-dir {root}/configs/{role}/%i
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=2
KillMode=mixed
TimeoutStopSec=30
LimitNOFILE=1048576
MemoryHigh={memory_high_mb}M
MemoryMax={memory_max_mb}M
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
"""
