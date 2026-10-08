#!/usr/bin/env python3
"""Remove instance-scoped active probe sessions from every data-plane role."""
from __future__ import annotations

import argparse
import json
import shlex

import deploy
import nb_release


def remote_control(path: str, command: str) -> str:
    script = ("import socket;" +
        f"s=socket.socket(socket.AF_UNIX);s.settimeout(5);s.connect({path!r});" +
        f"s.sendall(({command!r}+'\\n').encode());data=s.recv(32768);s.close();print(data.decode(),end='')")
    return "python3 -c " + shlex.quote(script)


def invoke(connection, role: str, worker: int, command: str) -> dict:
    path = deploy._control_socket_path(role, worker)
    raw = deploy.checked_run(connection, remote_control(path, command), tmo=15).strip()
    response = json.loads(raw.splitlines()[-1])
    if response.get("error"):
        raise RuntimeError(f"{role}[{worker}] probe command rejected: {response['error']}")
    return response


def preflight_all() -> list[dict]:
    results = []
    expected = f"{nb_release.NODE_PRODUCT_VERSION} ({nb_release.NODE_SEMANTIC_VERSION})"
    binary = f"{deploy.WORK}/shards/nb_node"
    for role in ("entry", "middle", "exit"):
        connection = deploy.connect(role)
        try:
            output = deploy.checked_run(connection, f"{shlex.quote(binary)} --version", tmo=15).strip()
            if expected not in output:
                raise RuntimeError(f"{role} shared runtime requires {expected}, got: {output}")
            results.append({"role": role, "version": expected})
        finally:
            connection.close()
    return results


def cleanup_all() -> list[dict]:
    results = []
    errors = []
    for role in ("entry", "middle", "exit"):
        try:
            connection = deploy.connect(role)
        except Exception as exc:
            errors.append(f"{role} connect failed: {exc}")
            continue
        try:
            for worker in range(deploy._effective_workers(role)):
                cleaned_count = 0
                try:
                    cleaned = invoke(connection, role, worker, "probe cleanup")
                    cleaned_count = int(cleaned.get("cleaned", 0))
                except Exception as exc:
                    errors.append(f"{role}[{worker}] cleanup failed: {exc}")
                try:
                    status = invoke(connection, role, worker, "probe status")
                    active = int(status.get("active", -1))
                    if active != 0:
                        errors.append(f"{role}[{worker}] still has {active} probe sessions")
                    results.append({"role": role, "worker": worker, "active": active,
                                    "cleaned": cleaned_count})
                except Exception as exc:
                    errors.append(f"{role}[{worker}] status failed: {exc}")
        finally:
            connection.close()
    if errors:
        raise RuntimeError("; ".join(errors))
    return results


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preflight", action="store_true")
    args = parser.parse_args()
    if args.preflight:
        print("PROBE_PREFLIGHT_JSON=" + json.dumps(preflight_all(), separators=(",", ":")))
    else:
        print("PROBE_CLEANUP_JSON=" + json.dumps(cleanup_all(), separators=(",", ":")))


if __name__ == "__main__":
    main()
