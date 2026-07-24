#!/usr/bin/env python3
"""NB 三节点逐角色重启与端到端恢复验收。"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import time
from types import SimpleNamespace

import deploy
import line_probe
import nb_diag
import nb_observe
from netem_matrix import atomic_report, run_case, validate_cluster


def role_pid(role: str) -> int:
    connection = deploy.connect(role)
    try:
        raw = deploy.run(connection, f"systemctl show -p MainPID --value {deploy._service_name(role)}").strip()
        return int(raw or 0)
    finally:
        connection.close()


def restart_role(role: str) -> tuple[float, int]:
    connection = deploy.connect(role)
    try:
        unit = deploy._service_name(role)
        started = time.monotonic()
        output = deploy.run(connection, f"systemctl restart {unit}; rc=$?; systemctl is-active {unit}; systemctl show -p MainPID --value {unit}; exit $rc", tmo=45)
        if "active" not in output.split():
            raise RuntimeError(f"{role} 重启后未进入 active: {output.strip()}")
        pids = [int(token) for token in output.split() if token.isdigit()]
        return started, (pids[-1] if pids else 0)
    finally:
        connection.close()


def wait_cluster(timeout_s: int, previous_pid: int, role: str) -> tuple[dict, float, int]:
    started = time.monotonic()
    last_error = ""
    while time.monotonic() - started < timeout_s:
        try:
            current_pid = role_pid(role)
            snapshot = nb_observe.collect()
            validate_cluster(snapshot)
            if current_pid > 0 and current_pid != previous_pid:
                return snapshot, time.monotonic() - started, current_pid
            last_error = f"PID 尚未切换: {previous_pid}->{current_pid}"
        except Exception as error:
            last_error = f"{type(error).__name__}: {error}"
        time.sleep(2)
    raise TimeoutError(f"{role} 在 {timeout_s}s 内未恢复: {last_error}")


def wait_data_plane(args, started: float) -> tuple[dict, float, list[dict]]:
    attempts = []
    deadline = started + args.recovery_timeout
    while time.monotonic() < deadline:
        at = time.monotonic() - started
        try:
            result = line_probe.run_integrity_probe(
                deploy._role_host("entry")["host"], args.socks_port,
                min(args.integrity_bytes, 64 * 1024), io_timeout=2)
            canary = line_probe.run_load_probe(
                deploy._role_host("entry")["host"], args.socks_port,
                min(args.target_mbps, 4.0), 5, io_timeout=3)
            attempts.append({"at_s": at, "passed": True})
            return {"integrity": result, "canary": canary}, time.monotonic() - started, attempts
        except Exception as error:
            attempts.append({"at_s": at, "passed": False,
                             "error": f"{type(error).__name__}: {error}"})
            time.sleep(0.5)
    raise TimeoutError(f"数据面在 {args.recovery_timeout}s 内未恢复: {attempts[-3:]}")


def main() -> int:
    parser = argparse.ArgumentParser(description="NB KZ 节点重启故障矩阵")
    parser.add_argument("--roles", default="exit,middle,entry")
    parser.add_argument("--recovery-timeout", type=int, default=90)
    parser.add_argument("--max-data-recovery", type=float, default=15.0)
    parser.add_argument("--cooldown", type=int, default=15)
    parser.add_argument("--load-duration", type=int, default=30)
    parser.add_argument("--target-mbps", type=float, default=4.0)
    parser.add_argument("--interval", type=int, default=5)
    parser.add_argument("--min-throughput-ratio", type=float, default=0.80)
    parser.add_argument("--max-queue-age-ms", type=int, default=1000)
    parser.add_argument("--integrity-bytes", type=int, default=256 * 1024)
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--continue-on-failure", action="store_true")
    args = parser.parse_args()
    roles = [role.strip() for role in args.roles.split(",") if role.strip()]
    if not roles or any(role not in ("entry", "middle", "exit") for role in roles):
        parser.error("--roles 只能包含 entry,middle,exit")
    if args.load_duration < 20 or args.load_duration > 600:
        parser.error("--load-duration 必须在 20..600 秒")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output or deploy.BUILD_DIR / "fault" / f"restart-{stamp}.json"
    report = {
        "schema_version": 1,
        "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "line": "-".join(deploy._role_host(role)["name"] for role in ("entry", "middle", "exit")),
        "roles": roles,
        "fec_required_mode": "observe-only",
        "cases": [],
        "status": "preflight-only" if not args.apply else "running",
    }
    try:
        baseline = nb_observe.collect()
        validate_cluster(baseline)
        report["baseline_integrity"] = line_probe.run_integrity_probe(
            deploy._role_host("entry")["host"], args.socks_port, args.integrity_bytes
        )
        atomic_report(output, report)
        if args.apply:
            for role in roles:
                case = {"role": role, "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")}
                old_pid = role_pid(role)
                case["pid_before"] = old_pid
                try:
                    restart_started, new_pid = restart_role(role)
                    case["process_recovery_s"] = time.monotonic() - restart_started
                    case["pid_after"] = new_pid
                    integrity, data_recovery_s, attempts = wait_data_plane(args, restart_started)
                    case["data_recovery_s"] = data_recovery_s
                    case["recovery_attempts"] = attempts
                    case["recovery_integrity"] = integrity
                    snapshot = nb_observe.collect()
                    validate_cluster(snapshot)
                    if new_pid <= 0 or new_pid == old_pid:
                        raise RuntimeError(f"{role} PID 未切换: {old_pid}->{new_pid}")
                    if args.cooldown:
                        time.sleep(args.cooldown)
                    probe_args = SimpleNamespace(**vars(args))
                    probe_args.duration = args.load_duration
                    probe_args.delay_ms = 0
                    probe_args.jitter_ms = 0
                    probe_args.reorder_pct = 0.0
                    case["probe"] = run_case(probe_args, 0.0, injected=False)
                    case["passed"] = case["probe"]["passed"] and data_recovery_s <= args.max_data_recovery
                    if data_recovery_s > args.max_data_recovery:
                        case["failure_reason"] = "data-recovery-sla"
                except Exception as error:
                    case["passed"] = False
                    case["error"] = f"{type(error).__name__}: {error}"
                    try:
                        restart_role(role)
                        wait_cluster(args.recovery_timeout, 0, role)
                    except Exception as recovery_error:
                        case["forced_recovery_error"] = f"{type(recovery_error).__name__}: {recovery_error}"
                    directory, archive, _ = nb_diag.incident_bundle()
                    case["incident_bundle"] = {"directory": str(directory), "archive": str(archive)}
                case["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
                report["cases"].append(case)
                atomic_report(output, report)
                print(json.dumps(case, ensure_ascii=False))
                if not case["passed"] and not args.continue_on_failure:
                    break
            report["status"] = "passed" if len(report["cases"]) == len(roles) and all(
                case["passed"] for case in report["cases"]
            ) else "failed"
    except Exception as error:
        report["status"] = "failed"
        report["fatal_error"] = f"{type(error).__name__}: {error}"
    report["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    atomic_report(output, report)
    print(f"节点故障矩阵报告: {output}")
    return 0 if report["status"] in ("passed", "preflight-only") else 1


if __name__ == "__main__":
    raise SystemExit(main())
