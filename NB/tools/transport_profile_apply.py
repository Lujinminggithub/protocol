#!/usr/bin/env python3
"""Push a role-local transport profile and issue one control-socket phase."""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import shlex

import deploy
import nb_shard_deploy


SAFE_LINE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")


def remote_command(control: str, command: str) -> str:
    script = (
        "import socket;"
        f"s=socket.socket(socket.AF_UNIX);s.settimeout(5);s.connect({control!r});"
        f"s.sendall(({command!r}+'\\n').encode());"
        "data=s.recv(8192);s.close();print(data.decode(),end='')"
    )
    return f"python3 -c {shlex.quote(script)}"


def invoke(role: str, command: str) -> list[dict]:
    connection = deploy.connect(role)
    responses = []
    try:
        for worker in range(deploy._effective_workers(role)):
            control = nb_shard_deploy.line_control_path(deploy.DEPLOY_INSTANCE, role, worker)
            raw = deploy.checked_run(connection, remote_command(control, command), tmo=15).strip()
            response = json.loads(raw.splitlines()[-1])
            if response.get("error"):
                raise RuntimeError(f"{role}[{worker}] rejected profile command: {response['error']}")
            responses.append({"worker": worker, "response": response})
    finally:
        connection.close()
    return responses


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("prepare", "commit", "abort", "rollback", "status"))
    parser.add_argument("--role", choices=("entry", "middle", "exit"), required=True)
    parser.add_argument("--line-id", required=True)
    parser.add_argument("--generation", type=int, default=0)
    parser.add_argument("--fingerprint", default="")
    parser.add_argument("--profile", type=pathlib.Path)
    args = parser.parse_args()
    if not deploy.DEPLOY_INSTANCE:
        raise SystemExit("NB_DEPLOY_INSTANCE is required")
    if not SAFE_LINE_ID.fullmatch(args.line_id):
        raise SystemExit("a safe line id is required")
    if args.phase != "status" and args.generation < 1:
        raise SystemExit("a positive generation is required")

    command = "profile status"
    remote_profile = ""
    if args.phase == "prepare":
        if args.profile is None or not args.profile.is_file() or len(args.fingerprint) != 16:
            raise SystemExit("prepare requires a profile and 16-digit fingerprint")
        directory = f"{deploy.INSTANCE_WORK}/transport-profiles"
        remote_profile = f"{directory}/{args.role}-{args.generation}.conf"
        connection = deploy.connect(args.role)
        try:
            deploy.checked_run(connection, f"mkdir -p {shlex.quote(directory)} && chmod 0700 {shlex.quote(directory)}")
            deploy.push_bytes(connection, args.profile.read_bytes(), remote_profile, mode=0o600)
        finally:
            connection.close()
        command = f"profile prepare {args.generation} {args.fingerprint} {args.line_id} {remote_profile}"
    elif args.phase in ("commit", "abort", "rollback"):
        command = f"profile {args.phase} {args.generation}"

    workers = invoke(args.role, command)
    if args.phase in ("commit", "rollback"):
        directory = f"{deploy.INSTANCE_WORK}/transport-profiles"
        source = f"{directory}/{args.role}-{args.generation}.conf"
        active = f"{directory}/{args.role}-active.conf"
        temporary = active + ".tmp"
        connection = deploy.connect(args.role)
        try:
            deploy.checked_run(connection, " && ".join((
                f"test -f {shlex.quote(source)}",
                f"cp -p {shlex.quote(source)} {shlex.quote(temporary)}",
                f"chmod 0600 {shlex.quote(temporary)}",
                f"mv -f {shlex.quote(temporary)} {shlex.quote(active)}",
            )))
        finally:
            connection.close()
    if args.phase == "status" and args.generation > 0:
        mismatched = [item["worker"] for item in workers
                      if int(item["response"].get("active_generation", 0)) != args.generation]
        if mismatched:
            raise RuntimeError(f"{args.role} profile generation mismatch workers={mismatched}")
        if args.fingerprint:
            mismatched = [item["worker"] for item in workers
                          if item["response"].get("active_fingerprint", "").lower() != args.fingerprint.lower()]
            if mismatched:
                raise RuntimeError(f"{args.role} profile fingerprint mismatch workers={mismatched}")
    result = {"role": args.role, "phase": args.phase, "generation": args.generation,
              "profile": remote_profile, "workers": workers}
    print("PROFILE_RESULT=" + json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    main()
