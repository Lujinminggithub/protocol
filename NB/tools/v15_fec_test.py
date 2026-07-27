#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import pathlib
import re
import shlex
import sys
import tempfile
import time
import traceback

from deploy import connect, run, launch, _service_exists, _service_name, WORK  # type: ignore


ROOT = pathlib.Path(__file__).resolve().parents[1]
LOG_DIR = pathlib.Path(tempfile.gettempdir()) / "nb_v15_fec_logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)

ENTRY_HOST = "106.75.169.83"
SOCKS_PORT = 1080
FEC_HTTP_PORT = 50021
PAYLOAD_URL = f"http://127.0.0.1:{FEC_HTTP_PORT}/fec-test.bin"

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


def remote_log_offsets() -> dict[str, int]:
    offsets = {}
    for role, path in (("middle", MIDDLE_LOG), ("exit", EXIT_LOG)):
        c = connect(role)
        raw = run(c, f"wc -c < {path} 2>/dev/null || echo 0", tmo=20).strip()
        c.close()
        offsets[role] = int(raw.splitlines()[-1] or 0)
    return offsets


def fetch_remote_log(role: str, remote_path: str, offset: int) -> pathlib.Path:
    c = connect(role)
    text = run(c, f"tail -c +{offset + 1} {remote_path} 2>/dev/null || true", tmo=60)
    c.close()
    out = LOG_DIR / f"{role}.log"
    out.write_text(text, encoding="utf-8", errors="ignore")
    return out


def prepare_payload_target(size: int = 1048576, filename: str = "fec-test.bin") -> str:
    c = connect("exit")
    target = f"{WORK}/www/{filename}"
    out = run(c, f"mkdir -p {WORK}/www && "
        f"[ $(stat -c %s {target} 2>/dev/null || echo 0) -eq {size} ] || head -c {size} /dev/urandom >{target}; "
        f"sha256sum {target} | awk '{{print $1}}'", tmo=30)
    listening = run(c, f"ss -H -ltn 'sport = :{FEC_HTTP_PORT}' | grep -q LISTEN; echo $?", tmo=10).strip()
    if not listening.endswith("0"):
        launch(c, f"cd {WORK}/www && exec setsid python3 -m http.server {FEC_HTTP_PORT} --bind 127.0.0.1 "
            "</dev/null >/tmp/nb-fec-http.log 2>&1", warmup=1.0)
        state = run(c, f"ss -H -ltn 'sport = :{FEC_HTTP_PORT}' | grep -q LISTEN; echo $?", tmo=10).strip()
        if not state.endswith("0"):
            c.close()
            raise RuntimeError("FEC payload target did not start")
    c.close()
    hashes = re.findall(r"\b[0-9a-f]{64}\b", out)
    if not hashes:
        raise RuntimeError(f"无法准备 FEC payload: {out}")
    return hashes[0]


def curl_payload(rounds: int = 2, expected_sha256: str | None = None, url: str = PAYLOAD_URL,
                 max_time: int = 30) -> list[float]:
    expected_sha256 = expected_sha256 or prepare_payload_target()
    username = os.environ.get("NB_SOCKS_USERNAME", "")
    password = os.environ.get("NB_SOCKS_PASSWORD", "")
    if not username or not password:
        raise RuntimeError("FEC payload probe requires SOCKS credentials")
    proxy_user = shlex.quote(f"{username}:{password}")
    c = connect("entry")
    times: list[float] = []
    try:
        for _ in range(rounds):
            cmd = (
                "tmp=$(mktemp); "
                f"meta=$(curl -sS --proxy-user {proxy_user} --socks5-hostname 127.0.0.1:{SOCKS_PORT} -o $tmp "
                f"-w '%{{http_code}} %{{time_total}}' {url} --max-time {max_time}); rc=$?; "
                "sha=$(sha256sum $tmp 2>/dev/null|awk '{print $1}'); rm -f $tmp; "
                "echo RC=$rc META=$meta SHA=$sha"
            )
            out = run(c, cmd, tmo=max_time + 15)
            m = re.search(r"RC=(\d+) META=(\d+) ([0-9.]+) SHA=([0-9a-f]+)", out)
            if not m or m.group(1) != "0" or m.group(2) != "200" or m.group(4) != expected_sha256:
                raise RuntimeError(f"payload 完整性失败: {out.strip()}")
            times.append(float(m.group(3)))
            time.sleep(0.5)
    finally:
        c.close()
    return times


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
        "NB_FEC_V15_ACTIVE": "on",
        "NB_FEC_V15_FORCE": "on",
        "NB_FEC_V15_FORCE_ROBUST": "on",
    }
    common_exit = {
        "NB_LOG_LEVEL": "DEBUG",
        "NB_FEC_V15": "on",
        "NB_FEC_V15_ACTIVE": "on",
    }
    if stage == "canary":
        middle_env = dict(common_middle)
        middle_env["NB_LOG_LEVEL"] = "INFO"
        middle_env.pop("NB_FEC_V15_FORCE_ROBUST", None)
        exit_env = dict(common_exit)
        exit_env["NB_LOG_LEVEL"] = "INFO"
    elif stage == "canary-robust":
        middle_env = dict(common_middle)
        middle_env["NB_LOG_LEVEL"] = "INFO"
        exit_env = dict(common_exit)
        exit_env["NB_LOG_LEVEL"] = "INFO"
    elif stage == "t1":
        middle_env = dict(common_middle)
        exit_env = dict(common_exit)
    elif stage == "t2":
        middle_env = dict(common_middle, NB_FEC_V15_DROP_SRC_MOD="4")
        exit_env = dict(common_exit, NB_FEC_V15_DROP_SRC_MOD="4")
    elif stage == "t3":
        middle_env = dict(common_middle, NB_FEC_V15_DROP_SRC_MOD="4", NB_FEC_V15_DROP_REPAIR_MOD="1")
        exit_env = dict(common_exit, NB_FEC_V15_DROP_SRC_MOD="4", NB_FEC_V15_DROP_REPAIR_MOD="1")
    elif stage == "auto":
        middle_env = {"NB_FEC_V15": "on", "NB_FEC_V15_ACTIVE": "off"}
        exit_env = {"NB_FEC_V15": "on", "NB_FEC_V15_ACTIVE": "off"}
    else:
        raise ValueError(stage)

    for role, env in (("middle", middle_env), ("exit", exit_env)):
        c = connect(role)
        if not _service_exists(c, role):
            c.close()
            raise RuntimeError(f"nb-{role}.service 不存在，请先执行安全部署")
        write_systemd_override(c, role, env)
        c.close()


def stage_t1() -> None:
    print("[RUN] T1 sidecar 拉起")
    apply_stage_env("t1")
    offsets = remote_log_offsets()
    sha = prepare_payload_target()
    curl_payload(2, sha)
    time.sleep(11)
    middle = fetch_remote_log("middle", MIDDLE_LOG, offsets["middle"]).read_text(encoding="utf-8", errors="ignore")
    exit_ = fetch_remote_log("exit", EXIT_LOG, offsets["exit"]).read_text(encoding="utf-8", errors="ignore")
    assert_marker("middle fec start", middle, r"fec v1\.5 start")
    assert_marker("middle fec ctrl ack", middle, r"fec ctrl ACK")
    assert_marker("middle fec v15 stat", middle, r"fec v15 stat:")
    assert_marker("exit fec ctrl start", exit_, r"fec ctrl START")
    assert_marker("exit fec v15 stat", exit_, r"fec v15 stat:")


def stage_t2() -> None:
    print("[RUN] T2 单缺片恢复")
    apply_stage_env("t2")
    offsets = remote_log_offsets()
    sha = prepare_payload_target()
    curl_payload(3, sha)
    time.sleep(11)
    middle = fetch_remote_log("middle", MIDDLE_LOG, offsets["middle"]).read_text(encoding="utf-8", errors="ignore")
    assert_marker("recover", middle, r"recovered=[1-9][0-9]*")


def stage_t3() -> None:
    print("[RUN] T3 NACK / RETX 回退")
    apply_stage_env("t3")
    offsets = remote_log_offsets()
    name = "fec-test-retransmit.bin"
    sha = prepare_payload_target(16384, name)
    curl_payload(4, sha, f"http://127.0.0.1:{FEC_HTTP_PORT}/{name}")
    time.sleep(11)
    middle = fetch_remote_log("middle", MIDDLE_LOG, offsets["middle"]).read_text(encoding="utf-8", errors="ignore")
    exit_ = fetch_remote_log("exit", EXIT_LOG, offsets["exit"]).read_text(encoding="utf-8", errors="ignore")
    nack = grep_expect(middle, r"nack=[1-9]")
    print(f"[INFO] middle nack: {nack}")
    if nack == 0:
        raise RuntimeError("缺少关键日志: nack")
    assert_marker("exit retx send", exit_, r"retx_sent=[1-9]")
    assert_marker("middle retx recv", middle, r"retx_recv=[1-9]")


def main() -> int:
    ap = argparse.ArgumentParser(description="执行 NB V1.5 FEC sidecar 自动化测试")
    ap.add_argument("stage", choices=["t1", "t2", "t3", "all", "auto"], help="执行阶段或恢复自动模式")
    args = ap.parse_args()

    restore_required = args.stage != "auto"
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
        elif args.stage == "auto":
            apply_stage_env("auto")
            print("[INFO] 已恢复观察模式(NB_FEC_V15=on, NB_FEC_V15_ACTIVE=off)")
        print("[OK] 自动化测试阶段完成")
        return 0
    except Exception as e:
        print(f"[ERR] {e!r}")
        traceback.print_exc()
        return 1
    finally:
        if restore_required:
            try:
                apply_stage_env("auto")
                print("[INFO] 已恢复观察模式(NB_FEC_V15=on, NB_FEC_V15_ACTIVE=off)")
            except Exception as restore_error:
                print(f"[ERR] 恢复观察模式失败: {restore_error!r}")
                raise


if __name__ == "__main__":
    raise SystemExit(main())
