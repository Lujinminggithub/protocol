#!/usr/bin/env python3
"""NB 线路开通探针：采集每跳 ICMP 与运行中 QUIC linkq，生成候选参数。

该工具只读且不自动应用参数。空闲或样本不足时会降低 confidence，产品控制面
必须在带负载探针和灰度通过后，才可将 candidate 提升为 active。
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import pathlib
import re
import socket
import statistics
import struct
import time

import deploy


LINK_RE = re.compile(
    r"rtt=(?P<rtt>[0-9.]+)ms.*?jit=(?P<jitter>[0-9.]+)ms "
    r"loss=(?P<loss>[0-9.]+)% dsent=(?P<sent>\d+).*?"
    r"spur=(?P<spurious>\d+).*?reorder=(?P<reorder_ms>[0-9.]+)ms/(?P<reorder_gap>\d+).*?"
    r"cwin=(?P<cwin_kb>\d+)KB.*?block=(?P<block>[01])/"
)
LINK_MTU_RE = re.compile(r"\bmtu=(?P<mtu>\d+)\b")
PING_LOSS_RE = re.compile(r"([0-9.]+)% packet loss")
PING_RTT_RE = re.compile(r"=\s*([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+)\s*ms")
PROBE_SINK_HOST = "nb-probe-sink.internal"
PROBE_ECHO_HOST = "nb-probe-echo.internal"
PROBE_PORT = 9
IPV4_UDP_OVERHEAD = 28
MTU_PROBE_MIN = 1280
MTU_PROBE_MAX = 1500
MTU_SAFETY_MARGIN = 48
MTU_RECOMMEND_MAX = 1500


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    at = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * p) - 1))
    return ordered[at]


def parse_ping(text: str) -> dict:
    loss = PING_LOSS_RE.search(text)
    rtt = PING_RTT_RE.search(text)
    return {
        "loss_pct": float(loss.group(1)) if loss else None,
        "rtt_min_ms": float(rtt.group(1)) if rtt else None,
        "rtt_avg_ms": float(rtt.group(2)) if rtt else None,
        "rtt_max_ms": float(rtt.group(3)) if rtt else None,
        "rtt_mdev_ms": float(rtt.group(4)) if rtt else None,
    }


def parse_linkq(text: str) -> list[dict]:
    samples = []
    for line in text.splitlines():
        match = LINK_RE.search(line)
        if not match:
            continue
        sample = {key: float(value) for key, value in match.groupdict().items()}
        sample["sent"] = int(sample["sent"])
        sample["spurious"] = int(sample["spurious"])
        sample["reorder_gap"] = int(sample["reorder_gap"])
        sample["cwin_kb"] = int(sample["cwin_kb"])
        sample["block"] = int(sample["block"])
        mtu = LINK_MTU_RE.search(line)
        sample["mtu"] = int(mtu.group("mtu")) if mtu else 0
        samples.append(sample)
    return samples


def summarize_linkq(samples: list[dict]) -> dict:
    valid = [sample for sample in samples if sample["sent"] >= 200]
    sent = sum(sample["sent"] for sample in valid)
    observed_mtu = [sample["mtu"] for sample in valid if sample.get("mtu", 0) > 0]
    return {
        "windows_total": len(samples),
        "windows_valid": len(valid),
        "packets_observed": sent,
        "rtt_p50_ms": percentile([x["rtt"] for x in valid], 0.50),
        "rtt_p95_ms": percentile([x["rtt"] for x in valid], 0.95),
        "rtt_p99_ms": percentile([x["rtt"] for x in valid], 0.99),
        "jitter_p95_ms": percentile([x["jitter"] for x in valid], 0.95),
        "effective_loss_mean_pct": statistics.fmean(x["loss"] for x in valid) if valid else 0.0,
        "effective_loss_p95_pct": percentile([x["loss"] for x in valid], 0.95),
        "reorder_gap_max": max((x["reorder_gap"] for x in valid), default=0),
        "reorder_delay_max_ms": max((x["reorder_ms"] for x in valid), default=0.0),
        "blocked_windows": sum(x["block"] for x in valid),
        "quic_udp_payload_mtu_min": min(observed_mtu, default=0),
        "quic_udp_payload_mtu_max": max(observed_mtu, default=0),
        "quic_ip_mtu_proven": min(observed_mtu, default=0) + IPV4_UDP_OVERHEAD
            if observed_mtu else 0,
    }


def recommend_mtu(summary: dict, mtu_probe: dict | None, current: dict) -> dict:
    quic_proven = int(summary.get("quic_ip_mtu_proven", 0) or 0)
    icmp_mtu = int((mtu_probe or {}).get("max_ip_mtu", 0) or 0)
    icmp_safe = max(MTU_PROBE_MIN, icmp_mtu - MTU_SAFETY_MARGIN) if icmp_mtu else 0
    evidence = [value for value in (quic_proven, icmp_safe) if value > 0]
    if evidence:
        recommended = min(MTU_RECOMMEND_MAX, max(evidence))
        confidence = "quic-and-df" if quic_proven and icmp_mtu else (
            "quic-proven" if quic_proven else "icmp-df-only")
    else:
        recommended = int(current.get("mtu_max", 0) or 0) or None
        confidence = "unavailable-keep-current"
    return {
        "mtu_max": recommended,
        "confidence": confidence,
        "quic_udp_payload_mtu": int(summary.get("quic_udp_payload_mtu_min", 0) or 0),
        "quic_ip_mtu_proven": quic_proven,
        "icmp_df_max_ip_mtu": icmp_mtu or None,
        "icmp_safety_margin_bytes": MTU_SAFETY_MARGIN,
    }


def recommend(summary: dict, current: dict, target_mbps: float,
              mtu_probe: dict | None = None) -> dict:
    rtt = max(summary["rtt_p95_ms"], 1.0)
    bdp = target_mbps * 1_000_000 / 8.0 * rtt / 1000.0
    cwin = int(max(256 * 1024, min(8 * 1024 * 1024, bdp * 2.0)))
    cwin = int(math.ceil(cwin / 65536.0) * 65536)

    observed_gap = summary["reorder_gap_max"]
    observed_delay = summary["reorder_delay_max_ms"]
    gap = max(3, int(math.ceil(observed_gap * 1.25)))
    delay_ms = max(2.0 * rtt, observed_delay * 1.25 + max(10.0, 0.25 * rtt))
    gap = min(1024, gap)
    delay_us = min(1_000_000, int(math.ceil(delay_ms) * 1000))

    loss = summary["effective_loss_p95_pct"]
    jitter = summary["jitter_p95_ms"]
    if rtt <= 30.0 and jitter <= 5.0 and loss <= 1.0:
        cc = "cubic"
    else:
        cc = "bbr"

    enough = summary["packets_observed"] >= 10_000 and summary["windows_valid"] >= 6
    mtu = recommend_mtu(summary, mtu_probe, current)
    calculated = {
        "cc": cc,
        "cwin_max_bytes": cwin if cc == "cubic" else None,
        "reorder_gap": gap,
        "reorder_delay_us": delay_us,
        "mtu_max": mtu["mtu_max"],
    }
    selected = calculated if enough else {
        "cc": current.get("cc"),
        "cwin_max_bytes": current.get("cwin_max_bytes"),
        "reorder_gap": current.get("reorder_gap"),
        "reorder_delay_us": current.get("reorder_delay_us"),
        "mtu_max": mtu["mtu_max"],
    }
    return {
        "confidence": "load-qualified" if enough else "insufficient-load",
        "auto_apply_allowed": False,
        **selected,
        "provisional": calculated,
        "target_mbps": target_mbps,
        "current": current,
        "mtu_evidence": mtu,
    }


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError(f"SOCKS 响应提前结束: {len(data)}/{size}")
        data.extend(chunk)
    return bytes(data)


def socks_connect(host: str, port: int, target: str) -> socket.socket:
    username = os.environ.get("NB_SOCKS_USERNAME", "").encode()
    password = os.environ.get("NB_SOCKS_PASSWORD", "").encode()
    if not username or not password or len(username) > 255 or len(password) > 255:
        raise RuntimeError("主动探针需要 NB_SOCKS_USERNAME 和 NB_SOCKS_PASSWORD")
    sock = socket.create_connection((host, port), timeout=15)
    sock.settimeout(30)
    sock.sendall(b"\x05\x01\x02")
    if recv_exact(sock, 2) != b"\x05\x02":
        sock.close(); raise RuntimeError("SOCKS 服务未接受用户名认证")
    sock.sendall(b"\x01" + bytes([len(username)]) + username + bytes([len(password)]) + password)
    if recv_exact(sock, 2) != b"\x01\x00":
        sock.close(); raise RuntimeError("SOCKS 用户认证失败")
    encoded = target.encode("ascii")
    sock.sendall(b"\x05\x01\x00\x03" + bytes([len(encoded)]) + encoded + struct.pack("!H", PROBE_PORT))
    response = recv_exact(sock, 4)
    if response[0] != 5 or response[1] != 0:
        sock.close(); raise RuntimeError(f"SOCKS CONNECT 被拒绝: reply={response[1]}")
    atyp = response[3]
    if atyp == 1:
        recv_exact(sock, 6)
    elif atyp == 3:
        recv_exact(sock, recv_exact(sock, 1)[0] + 2)
    elif atyp == 4:
        recv_exact(sock, 18)
    else:
        sock.close(); raise RuntimeError(f"SOCKS 返回未知 ATYP={atyp}")
    return sock


def run_integrity_probe(entry_host: str, socks_port: int, size: int = 256 * 1024) -> dict:
    payload = bytes((index * 31 + 17) & 0xff for index in range(4096))
    expected = (payload * math.ceil(size / len(payload)))[:size]
    sock = socks_connect(entry_host, socks_port, PROBE_ECHO_HOST)
    started = time.monotonic(); sock.sendall(expected); sock.shutdown(socket.SHUT_WR)
    received = bytearray()
    while True:
        chunk = sock.recv(65536)
        if not chunk: break
        received.extend(chunk)
    elapsed = time.monotonic() - started; sock.close()
    if bytes(received) != expected:
        raise RuntimeError(f"主动探针完整性失败: sent={len(expected)} recv={len(received)}")
    return {"bytes": size, "elapsed_s": elapsed, "integrity": "ok"}


def run_load_probe(entry_host: str, socks_port: int, target_mbps: float, duration_s: int) -> dict:
    sock = socks_connect(entry_host, socks_port, PROBE_SINK_HOST)
    chunk = bytes((index * 13 + 29) & 0xff for index in range(16 * 1024))
    target_bytes = int(target_mbps * 1_000_000 / 8.0 * duration_s)
    started = time.monotonic(); sent = 0
    while sent < target_bytes:
        now = time.monotonic()
        allowed = int(target_mbps * 1_000_000 / 8.0 * (now - started + 0.02))
        if sent >= allowed:
            time.sleep(min(0.005, (sent - allowed + 1) * 8.0 / (target_mbps * 1_000_000)))
            continue
        count = min(len(chunk), target_bytes - sent, max(1, allowed - sent))
        sock.sendall(chunk[:count]); sent += count
    sock.shutdown(socket.SHUT_WR)
    response = bytearray()
    while True:
        data = sock.recv(4096)
        if not data: break
        response.extend(data)
    elapsed = time.monotonic() - started; sock.close()
    match = re.search(rb"NBPROBE OK bytes=(\d+)", response)
    received = int(match.group(1)) if match else -1
    if received != sent:
        raise RuntimeError(f"主动探针计数失败: sent={sent} exit={received} response={response[:100]!r}")
    return {"bytes": sent, "elapsed_s": elapsed,
        "achieved_mbps": sent * 8.0 / elapsed / 1_000_000.0, "integrity": "count-ok"}


def log_offset(role: str) -> int:
    connection = deploy.connect(role)
    value = deploy.run(connection, f"wc -c < {deploy.WORK}/logs/nb-{role}.log").strip()
    connection.close()
    return int(value or 0)


def log_since(role: str, offset: int) -> str:
    connection = deploy.connect(role)
    text = deploy.run(connection, f"tail -c +{offset + 1} {deploy.WORK}/logs/nb-{role}.log")
    connection.close()
    return text


def probe_path_mtu(source_role: str, target: str,
                   minimum: int = MTU_PROBE_MIN,
                   maximum: int = MTU_PROBE_MAX) -> dict:
    """使用 IPv4 DF ICMP 探测路径 MTU 下界；失败不覆盖运行中的配置。"""
    connection = deploy.connect(source_role)
    attempts = []
    low, high = minimum, maximum
    base_ok = False
    while low <= high:
        candidate = (low + high) // 2
        payload = candidate - IPV4_UDP_OVERHEAD
        command = (f"ping -4 -n -q -M do -c 2 -W 1 -s {payload} {target} "
                   ">/dev/null 2>&1")
        output = deploy.run(connection, f"{command}; echo rc=$?", tmo=10)
        ok = "rc=0" in output.splitlines()
        attempts.append({"ip_mtu": candidate, "icmp_payload": payload, "ok": ok})
        if ok:
            base_ok = True
            low = candidate + 1
        else:
            high = candidate - 1
    connection.close()
    return {
        "method": "icmp-ipv4-df",
        "status": "ok" if base_ok else "unavailable",
        "max_ip_mtu": high if base_ok else None,
        "search_min": minimum,
        "search_max": maximum,
        "attempts": attempts,
    }


def collect_segment(source_role: str, target: str, samples: int, target_mbps: float) -> dict:
    connection = deploy.connect(source_role)
    ping = deploy.run(connection, f"ping -n -q -c {samples} -i 0.1 -W 2 {target}", tmo=max(30, samples // 2))
    log_path = f"{deploy.WORK}/logs/nb-{source_role}.log"
    log = deploy.run(connection,
        f"tac {log_path} 2>/dev/null | sed -n '1,/runtime: core dump enabled/p' | tac | "
        "grep 'linkq pool' | tail -120")
    connection.close()
    linkq = summarize_linkq(parse_linkq(log))
    current = deploy.LAB.get("transport", {}).get(source_role, {})
    mtu = probe_path_mtu(source_role, target)
    return {
        "source_role": source_role,
        "target": target,
        "icmp": parse_ping(ping),
        "mtu": mtu,
        "quic": linkq,
        "candidate": recommend(linkq, current, target_mbps, mtu),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ping-samples", type=int, default=30)
    parser.add_argument("--target-mbps", type=float, default=10.0)
    parser.add_argument("--active", action="store_true", help="通过真实三跳 QUIC 产生受控负载")
    parser.add_argument("--duration", type=int, default=90, help="主动负载持续秒数")
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.ping_samples < 10 or args.ping_samples > 300:
        raise SystemExit("--ping-samples 必须在 10..300")
    if args.target_mbps <= 0.1 or args.target_mbps > 1000:
        raise SystemExit("--target-mbps 必须在 0.1..1000")
    if args.duration < 10 or args.duration > 600:
        raise SystemExit("--duration 必须在 10..600")

    middle = deploy._role_host("middle")
    exit_host = deploy._role_host("exit")
    middle_target = middle.get("private_ip") or middle["host"]
    active_result = None
    active_logs = None
    if args.active:
        offsets = {role: log_offset(role) for role in ("entry", "middle")}
        integrity = run_integrity_probe(deploy._role_host("entry")["host"], args.socks_port)
        load = run_load_probe(deploy._role_host("entry")["host"], args.socks_port,
            args.target_mbps, args.duration)
        time.sleep(12)
        active_logs = {role: log_since(role, offsets[role]) for role in offsets}
        active_result = {"integrity": integrity, "load": load}

    entry_segment = collect_segment("entry", middle_target, args.ping_samples, args.target_mbps)
    middle_segment = collect_segment("middle", exit_host["host"], args.ping_samples, args.target_mbps)
    if active_logs is not None:
        for role, segment in (("entry", entry_segment), ("middle", middle_segment)):
            summary = summarize_linkq(parse_linkq(active_logs[role]))
            segment["quic"] = summary
            segment["candidate"] = recommend(summary,
                deploy.LAB.get("transport", {}).get(role, {}), args.target_mbps,
                segment.get("mtu"))

    result = {
        "schema_version": 2,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "line": f"{deploy._role_host('entry')['name']}-{middle['name']}-{exit_host['name']}",
        "fixed_exit": exit_host["name"],
        "probe_mode": "active-quic" if args.active else "passive-runtime",
        "status": "candidate",
        "active_probe": active_result,
        "segments": {
            "entry_middle": entry_segment,
            "middle_exit": middle_segment,
        },
        "notes": [
            "该结果不会自动修改线上配置。",
            "insufficient-load 表示必须补带负载 QUIC 探针和灰度。",
            "动态选路只对新会话生效，固定出口不变。",
            "MTU 推荐同时参考 IPv4 DF 与运行中 QUIC payload，证据不足时保留当前配置。",
        ],
    }
    output = args.output or pathlib.Path("build") / "line-profile-candidate.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    print(f"候选参数已写入: {output}")


if __name__ == "__main__":
    main()
