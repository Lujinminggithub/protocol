#!/usr/bin/env python3
"""本地单进程多线路 smoke：验证 C 后端无需多实例即可预热与切 active line。"""

import json
import pathlib
import socket
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
TMP = ROOT / "tmp" / "single-process-multiline-smoke"


def free_udp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])
    finally:
        s.close()


def wait_log(path: pathlib.Path, needle: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(encoding="utf-8", errors="ignore"):
            return True
        time.sleep(0.1)
    return False


def main() -> int:
    TMP.mkdir(parents=True, exist_ok=True)
    control = TMP / "route-control.txt"
    metrics = TMP / "route-metrics.json"
    conf = TMP / "ingress.conf"
    log = TMP / "ingress.log"
    listen_port = free_udp_port()
    primary_relay = free_udp_port()
    backup_relay = free_udp_port()
    if control.exists():
        control.unlink()
    if metrics.exists():
        metrics.unlink()
    conf.write_text(
        "\n".join(
            [
                "node_name=ingress-single-process",
                "role=ingress",
                "hop_name=gz",
                "transport=udp",
                "listen_host=127.0.0.1",
                "auth_token=",
                "connect_type=native",
                "default_line=primary",
                f"route_control_path={control}",
                f"route_metrics_path={metrics}",
                "enable_summary_dump=true",
                "line_drain_timeout_sec=1",
                f"line.primary=path=gz@ingress=127.0.0.1:{listen_port},hk@relay=127.0.0.1:{primary_relay};priority=100;enabled=true",
                f"line.backup=path=gz@ingress=127.0.0.1:{listen_port},hk2@relay=127.0.0.1:{backup_relay};priority=80;enabled=true",
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    exe = ROOT / "xgw.exe"
    if not exe.exists():
        exe = ROOT / "xgw"
    with log.open("w", encoding="utf-8") as out:
        proc = subprocess.Popen([str(exe), "run", str(conf)], stdout=out, stderr=subprocess.STDOUT, cwd=str(ROOT))
    try:
        if not wait_log(log, "runtime.routes active=primary", 5):
            raise RuntimeError("runtime did not start with primary line")
        control.write_text("PREWARM_LINE backup\n", encoding="utf-8")
        if not wait_log(log, "route.prewarm line=backup", 5):
            raise RuntimeError("backup line was not prewarmed")
        time.sleep(0.2)
        control.write_text("SET_ACTIVE_LINE backup\n", encoding="utf-8")
        if not wait_log(log, "route.active.request line=backup", 5):
            raise RuntimeError("backup active request was not accepted")
        if not wait_log(log, "route.active line=backup", 5):
            raise RuntimeError("backup line did not become active")
        if not wait_log(log, "route.drain old_line=primary", 5):
            raise RuntimeError("primary line did not enter drain")
        deadline = time.time() + 5
        metrics_data = {}
        while time.time() < deadline:
            if metrics.exists():
                metrics_data = json.loads(metrics.read_text(encoding="utf-8"))
                if metrics_data.get("backup", {}).get("active") is True:
                    break
            time.sleep(0.1)
        if metrics_data.get("backup", {}).get("active") is not True:
            raise RuntimeError("route metrics did not expose backup active state")
        summary = {
            "ok": True,
            "config": str(conf),
            "log": str(log),
            "control": str(control),
            "metrics": str(metrics),
            "metrics_data": metrics_data,
            "listen_port": listen_port,
            "primary_relay": primary_relay,
            "backup_relay": backup_relay,
        }
        (TMP / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0
    except Exception as exc:
        text = log.read_text(encoding="utf-8", errors="ignore") if log.exists() else ""
        print(f"single-process multiline smoke failed: {exc}", file=sys.stderr)
        print(text[-4000:], file=sys.stderr)
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
