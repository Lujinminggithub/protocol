#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import time
import traceback

from deploy import connect, run, launch, _service_exists, _service_name, WORK, _legacy_start_cmd, _whitelist_remote  # type: ignore


ROOT = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = pathlib.Path(tempfile.gettempdir()) / "nb_v15_fec_logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)

ENTRY_HOST = "106.75.169.83"
SOCKS_PORT = 1080
TRAFFIC_URL = "https://www.baidu.com"

MIDDLE_LOG = f"{WORK}/logs/nb-middle.log"
EXIT_LOG = f"{WORK}/logs/nb-exit.log"
ENTRY_LOG = f"{WORK}/logs/nb-entry.log"


def write_systemd_override(c, role: str, env: dict[str, str]) -> None:
    dropin = f"/etc/systemd/system/nb-{role}.service.d/override.conf"
    lines = ["[Service]"]
    for k, v in env.items():
        lines.append(f"Environment={k}={v}")
    content = "\n".join(lines) + "\n"
    cmd = (
        f"mkdir -p /etc/systemd/system/nb-{role}.service.d && "
        f"cat > {dropin} <<'EOF'\n{content}EOF\n"
        f"systemctl daemon-reload && systemctl restart {_service_name(role)}"
    )
    out = run(c, cmd, tmo=90)
    if "Failed" in out or "error" in out.lower():
        raise RuntimeError(f"{role} 写入 systemd drop-in 失败:\n{out}")


def restart_legacy_with_env(c, role: str, env: dict[str, str]) -> None:
    env_prefix = " ".join(f"{k}={v}" for k, v in env.items())
    if role == "exit":
        base = _legacy_start_cmd("exit", wl_remote=_whitelist_remote())
    elif role == "middle":
        base = _legacy_start_cmd("middle", fec_enabled=False)
    else:
        raise RuntimeError(f"自动化测试目前只重启 middle/exit，不支持 role={role}")
    cmd = base.replace("setsid nohup ", f"setsid nohup env {env_prefix} ", 1)
    out = run(c, "pkill -9 -x nb_node 2>/dev/null || true", tmo=20)
    _ = out
    launch(c, cmd, warmup=3.0)
    time.sleep(3.0)
    proc = run(c, "pgrep -ax nb_node 2>/dev/null | tail -1", tmo=20).strip()
    if "nb_node" not in proc:
        raise RuntimeError(f"{role} legacy 重启失败，未发现 nb_node 进程")


def clear_remote_logs() -> None:
    for role, path in (("entry", ENTRY_LOG), ("middle", MIDDLE_LOG), ("exit", EXIT_LOG)):
        c = connect(role)
        run(c, f": > {path}", tmo=20)
        c.close()


def fetch_remote_log(role: str, remote_path: str) -> pathlib.Path:
    c = connect(role)
    text = run(c, f"cat {remote_path} 2>/dev/null || true", tmo=60)
    c.close()
    out = LOG_DIR / f"{role}.log"
    out.write_text(text, encoding="utf-8", errors="ignore")
    return out


def curl_traffic(rounds: int = 2) -> None:
    curl = "curl"
    for _ in range(rounds):
        cmd = [
            curl,
            "--socks5",
            f"{ENTRY_HOST}:{SOCKS_PORT}",
            "-o",
            "NUL" if sys.platform.startswith("win") else "/dev/null",
            "-L",
            "--max-time",
            "25",
            TRAFFIC_URL,
        ]
        subprocess.run(cmd, check=False, capture_output=True, text=True)
        time.sleep(1.0)


def grep_expect(text: str, pattern: str) -> int:
    return len(re.findall(pattern, text))


def assert_marker(name: str, text: str, pattern: str) -> None:
    n = grep_expect(text, pattern)
    print(f"[INFO] {name}: {n}")
    if n == 0:
        raise RuntimeError(f"缺少关键日志: {name}")


def apply_stage_env(stage: str) -> None:
    common_middle = {
        "NB_LOG_LEVEL": "DEBUG",
        "NB_FEC_V15": "on",
        "NB_FEC_V15_FORCE": "on",
    }
    common_exit = {
        "NB_LOG_LEVEL": "DEBUG",
        "NB_FEC_V15": "on",
    }
    if stage == "t1":
        middle_env = dict(common_middle)
        exit_env = dict(common_exit)
    elif stage == "t2":
        middle_env = dict(common_middle, NB_FEC_V15_DROP_SRC_MOD="1")
        exit_env = dict(common_exit, NB_FEC_V15_DROP_SRC_MOD="1")
    elif stage == "t3":
        middle_env = dict(common_middle, NB_FEC_V15_DROP_SRC_MOD="1", NB_FEC_V15_DROP_REPAIR_MOD="1")
        exit_env = dict(common_exit, NB_FEC_V15_DROP_SRC_MOD="1", NB_FEC_V15_DROP_REPAIR_MOD="1")
    elif stage == "auto":
        middle_env = {"NB_FEC_V15": "on"}
        exit_env = {"NB_FEC_V15": "on"}
    else:
        raise ValueError(stage)

    for role, env in (("middle", middle_env), ("exit", exit_env)):
        c = connect(role)
        if _service_exists(c, role):
            write_systemd_override(c, role, env)
        else:
            restart_legacy_with_env(c, role, env)
        c.close()


def stage_t1() -> None:
    print("[RUN] T1 sidecar 拉起")
    apply_stage_env("t1")
    clear_remote_logs()
    curl_traffic(2)
    middle = fetch_remote_log("middle", MIDDLE_LOG).read_text(encoding="utf-8", errors="ignore")
    exit_ = fetch_remote_log("exit", EXIT_LOG).read_text(encoding="utf-8", errors="ignore")
    assert_marker("middle fec start", middle, r"fec v1\.5 start")
    assert_marker("middle fec ctrl ack", middle, r"fec ctrl ACK")
    assert_marker("middle fec v15 stat", middle, r"fec v15 stat:")
    assert_marker("exit fec ctrl start", exit_, r"fec ctrl START")
    assert_marker("exit fec v15 stat", exit_, r"fec v15 stat:")


def stage_t2() -> None:
    print("[RUN] T2 单缺片恢复")
    apply_stage_env("t2")
    clear_remote_logs()
    curl_traffic(3)
    exit_ = fetch_remote_log("exit", EXIT_LOG).read_text(encoding="utf-8", errors="ignore")
    assert_marker("drop", exit_, r"fec test drop")
    assert_marker("recover", exit_, r"fec recover block")


def stage_t3() -> None:
    print("[RUN] T3 NACK / RETX 回退")
    apply_stage_env("t3")
    clear_remote_logs()
    curl_traffic(4)
    middle = fetch_remote_log("middle", MIDDLE_LOG).read_text(encoding="utf-8", errors="ignore")
    exit_ = fetch_remote_log("exit", EXIT_LOG).read_text(encoding="utf-8", errors="ignore")
    nack_total = grep_expect(middle, r"fec nack") + grep_expect(exit_, r"fec nack")
    print(f"[INFO] nack(total): {nack_total}")
    if nack_total == 0:
        raise RuntimeError("缺少关键日志: nack")
    assert_marker("retx send", middle, r"fec retx send")
    assert_marker("retx recv", exit_, r"fec retx recv")


def main() -> int:
    ap = argparse.ArgumentParser(description="执行 NB V1.5 FEC sidecar 自动化测试")
    ap.add_argument("stage", choices=["t1", "t2", "t3", "all", "auto"], help="执行阶段或恢复自动模式")
    args = ap.parse_args()

    try:
        if args.stage == "t1":
            stage_t1()
        elif args.stage == "t2":
            stage_t2()
        elif args.stage == "t3":
            stage_t3()
        elif args.stage == "all":
            stage_t1()
            stage_t2()
            stage_t3()
            apply_stage_env("auto")
            print("[INFO] 已恢复自动模式(NB_FEC_V15=on, 无 force/drop)")
        elif args.stage == "auto":
            apply_stage_env("auto")
            print("[INFO] 已恢复自动模式(NB_FEC_V15=on, 无 force/drop)")
        print("[OK] 自动化测试阶段完成")
        return 0
    except Exception as e:
        print(f"[ERR] {e!r}")
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
