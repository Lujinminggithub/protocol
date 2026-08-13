#!/usr/bin/env python3
"""NB 线路开通探针：采集每跳 ICMP 与运行中 QUIC linkq，生成候选参数。

该工具只读且不自动应用参数。空闲或样本不足时会降低 confidence，产品控制面
必须在带负载探针和灰度通过后，才可将 candidate 提升为 active。
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import pathlib
import re
import shlex
import socket
import statistics
import struct
import threading
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
PROBE_SOURCE_HOST = "nb-probe-source.internal"
PROBE_PORT = 9
IPV4_UDP_OVERHEAD = 28
MTU_PROBE_MIN = 1280
MTU_PROBE_MAX = 1500
MTU_SAFETY_MARGIN = 48
MTU_RECOMMEND_MAX = 1500

ENTRY_LOCAL_PROBE_SCRIPT = r'''
import json
import math
import re
import socket
import struct
import sys
import time

request = json.load(sys.stdin)

def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("SOCKS response ended early")
        data.extend(chunk)
    return bytes(data)

def fnv1a64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value

def connect_socks(target):
    username = request["username"].encode()
    password = request["password"].encode()
    sock = socket.create_connection(("127.0.0.1", request["socks_port"]), timeout=15)
    sock.settimeout(request["io_timeout"])
    sock.sendall(b"\x05\x01\x02")
    if recv_exact(sock, 2) != b"\x05\x02":
        raise RuntimeError("SOCKS authentication method rejected")
    sock.sendall(b"\x01" + bytes([len(username)]) + username + bytes([len(password)]) + password)
    if recv_exact(sock, 2) != b"\x01\x00":
        raise RuntimeError("SOCKS credentials rejected")
    encoded = target.encode("ascii")
    sock.sendall(b"\x05\x01\x00\x03" + bytes([len(encoded)]) + encoded + struct.pack("!H", 9))
    response = recv_exact(sock, 4)
    if response[0] != 5 or response[1] != 0:
        raise RuntimeError("SOCKS CONNECT rejected: reply=%d" % response[1])
    atyp = response[3]
    if atyp == 1:
        recv_exact(sock, 6)
    elif atyp == 3:
        recv_exact(sock, recv_exact(sock, 1)[0] + 2)
    elif atyp == 4:
        recv_exact(sock, 18)
    else:
        raise RuntimeError("unknown SOCKS address type")
    return sock

mode = request["mode"]
if mode == "integrity":
    size = request["size"]
    seed = bytes((index * 31 + 17) & 0xff for index in range(4096))
    expected = (seed * int(math.ceil(float(size) / len(seed))))[:size]
    expected_hash = fnv1a64(expected)
    sock = connect_socks("nb-probe-sink.internal")
    started = time.monotonic()
    sock.sendall(struct.pack("!4sQ", b"NBP1", len(expected)))
    sock.sendall(expected)
    response = bytearray()
    while b"NBPROBE OK " not in response:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response.extend(chunk)
    elapsed = time.monotonic() - started
    sock.close()
    match = re.search(rb"NBPROBE OK bytes=(\d+) hash=([0-9a-fA-F]{16})", response)
    received = int(match.group(1)) if match else -1
    received_hash = int(match.group(2), 16) if match else -1
    if received != len(expected) or received_hash != expected_hash:
        raise RuntimeError("integrity mismatch: sent=%d received=%d" % (len(expected), received))
    result = {"bytes": size, "elapsed_s": elapsed, "integrity": "ok", "origin": "entry-local"}
elif mode == "downlink":
    target_mbps = request["target_mbps"]
    duration_s = request["duration_s"]
    target_bytes = int(target_mbps * 1000000.0 / 8.0 * duration_s)
    sock = connect_socks("nb-probe-source.internal")
    sock.sendall(struct.pack("!4sQ", b"NBP2", target_bytes))
    started = time.monotonic()
    received = 0
    value = 14695981039346656037
    while received < target_bytes:
        data = sock.recv(min(64 * 1024, target_bytes - received))
        if not data:
            break
        expected = bytes((((received + index) * 13 + 29) & 0xff) for index in range(len(data)))
        if data != expected:
            raise RuntimeError("downlink integrity mismatch at offset=%d" % received)
        for byte in data:
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        received += len(data)
    elapsed = time.monotonic() - started
    sock.close()
    if received != target_bytes:
        raise RuntimeError("downlink count mismatch: expected=%d received=%d" % (target_bytes, received))
    result = {"bytes": received, "elapsed_s": elapsed,
        "planned_duration_s": duration_s, "achieved_mbps": received * 8.0 / max(elapsed, 0.001) / 1000000.0,
        "integrity": "count-ok", "hash": "%016x" % value, "origin": "entry-local"}
elif mode == "load":
    target_mbps = request["target_mbps"]
    duration_s = request["duration_s"]
    target_bytes = int(target_mbps * 1000000.0 / 8.0 * duration_s)
    chunk = bytes((index * 13 + 29) & 0xff for index in range(16 * 1024))
    sock = connect_socks("nb-probe-sink.internal")
    sock.settimeout(min(1.0, request["io_timeout"]))
    sock.sendall(struct.pack("!4sQ", b"NBP1", target_bytes))
    started = time.monotonic()
    deadline = started + duration_s
    next_progress = started + 5.0
    sent = 0
    while sent < target_bytes and time.monotonic() < deadline:
        now = time.monotonic()
        allowed = int(target_mbps * 1000000.0 / 8.0 * min(duration_s, now - started + 0.02))
        if sent >= allowed:
            time.sleep(min(0.005, max(0.0, deadline - now)))
            continue
        count = min(len(chunk), target_bytes - sent, max(1, allowed - sent))
        try:
            written = sock.send(chunk[:count])
        except socket.timeout:
            written = 0
        if written:
            sent += written
        now = time.monotonic()
        if now >= next_progress:
            elapsed = max(0.001, now - started)
            print("NBPROBE_PROGRESS " + json.dumps({"elapsed_s": elapsed,
                "sent_bytes": sent, "achieved_mbps": sent * 8.0 / elapsed / 1000000.0}), flush=True)
            next_progress = now + 5.0
    send_elapsed = time.monotonic() - started
    sock.shutdown(socket.SHUT_WR)
    sock.settimeout(request["io_timeout"])
    response = bytearray()
    while b"NBPROBE OK " not in response:
        data = sock.recv(4096)
        if not data:
            break
        response.extend(data)
    elapsed = time.monotonic() - started
    sock.close()
    match = re.search(rb"NBPROBE OK bytes=(\d+)", response)
    received = int(match.group(1)) if match else -1
    if received != sent:
        raise RuntimeError("load count mismatch: sent=%d exit=%d" % (sent, received))
    result = {"bytes": sent, "elapsed_s": elapsed, "send_elapsed_s": send_elapsed,
        "planned_duration_s": duration_s, "achieved_mbps": sent * 8.0 / max(elapsed, 0.001) / 1000000.0,
        "integrity": "count-ok", "origin": "entry-local"}
else:
    raise RuntimeError("unknown probe mode")

print("NBPROBE_RESULT " + json.dumps(result), flush=True)
'''


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    at = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * p) - 1))
    return ordered[at]


def winsorized_percentile(values: list[float], p: float) -> float:
    """Ignore one isolated high window once there is enough probe coverage."""
    ordered = sorted(values)
    if len(ordered) >= 6:
        ordered[-1] = ordered[-2]
    return percentile(ordered, p)


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
        "reorder_percentile_windows": len(valid),
        "reorder_gap_p95": winsorized_percentile([x["reorder_gap"] for x in valid], 0.95),
        "reorder_delay_p95_ms": winsorized_percentile([x["reorder_ms"] for x in valid], 0.95),
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
    current_cwin = int(current.get("cwin_max_bytes", 0) or 0)
    if current.get("cc") == "cubic":
        cwin = max(cwin, current_cwin)

    observed_gap = summary.get("reorder_gap_p95", summary["reorder_gap_max"])
    observed_delay = summary.get("reorder_delay_p95_ms", summary["reorder_delay_max_ms"])
    gap = min(64, max(8, int(math.ceil(observed_gap)) + 8))
    delay_cap_ms = 80.0 if rtt <= 30.0 else 120.0
    delay_ms = min(delay_cap_ms, max(20.0,
        observed_delay + 3.0 * summary["jitter_p95_ms"]))
    delay_us = int(math.ceil(delay_ms) * 1000)

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


def evaluate_admission(active_probe: dict | None, target_mbps: float,
                       downstream_target_mbps: float | None = None,
                       min_throughput_ratio: float = 0.90) -> dict:
    reasons = []
    active_probe = active_probe or {}
    integrity = active_probe.get("integrity") or {}
    uplink = active_probe.get("uplink") or active_probe.get("load") or {}
    downlink = active_probe.get("downlink") or uplink
    achieved_up = float(uplink.get("achieved_mbps", 0) or 0)
    achieved_down = float(downlink.get("achieved_mbps", 0) or 0)
    if integrity.get("integrity") != "ok":
        reasons.append("payload-integrity")
    if uplink.get("integrity") != "count-ok":reasons.append("uplink-integrity")
    if downlink.get("integrity") != "count-ok":reasons.append("downlink-integrity")
    down_target=downstream_target_mbps if downstream_target_mbps is not None else target_mbps
    if achieved_up < target_mbps * min_throughput_ratio:reasons.append("insufficient-uplink")
    if achieved_down < down_target * min_throughput_ratio:reasons.append("insufficient-downlink")
    return {
        "status": "admitted" if not reasons else "rejected",
        "reasons": reasons,
        "target_mbps": min(target_mbps,down_target),
        "target_upstream_mbps":target_mbps,"target_downstream_mbps":down_target,
        "achieved_mbps": min(achieved_up,achieved_down),
        "achieved_upstream_mbps": achieved_up,"achieved_downstream_mbps": achieved_down,
        "throughput_ratio": min(achieved_up/target_mbps if target_mbps>0 else 0,
            achieved_down/down_target if down_target>0 else 0),
        "minimum_throughput_ratio": min_throughput_ratio,
    }
def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError(f"SOCKS 响应提前结束: {len(data)}/{size}")
        data.extend(chunk)
    return bytes(data)


def fnv1a64(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


class EntrySSHSocket:
    def __init__(self, port: int):
        self.client = deploy.connect("entry")
        try:
            self.channel = self.client.get_transport().open_channel(
                "direct-tcpip", ("127.0.0.1", port), ("127.0.0.1", 0))
        except Exception:
            self.client.close()
            raise

    def settimeout(self, value): self.channel.settimeout(value)
    def sendall(self, value): return self.channel.sendall(value)
    def recv(self, size): return self.channel.recv(size)
    def shutdown(self, how): return self.channel.shutdown(how)
    def close(self):
        try: self.channel.close()
        finally: self.client.close()


def socks_connect(host: str, port: int, target: str, io_timeout: float = 30) -> socket.socket:
    username = os.environ.get("NB_SOCKS_USERNAME", "").encode()
    password = os.environ.get("NB_SOCKS_PASSWORD", "").encode()
    if not username or not password or len(username) > 255 or len(password) > 255:
        raise RuntimeError("主动探针需要 NB_SOCKS_USERNAME 和 NB_SOCKS_PASSWORD")
    sock = (EntrySSHSocket(port) if os.environ.get("NB_PROBE_VIA_ENTRY_SSH") == "1"
            else socket.create_connection((host, port), timeout=15))
    sock.settimeout(io_timeout)
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


def run_integrity_probe(entry_host: str, socks_port: int, size: int = 256 * 1024,
                        io_timeout: float = 30) -> dict:
    payload = bytes((index * 31 + 17) & 0xff for index in range(4096))
    expected = (payload * math.ceil(size / len(payload)))[:size]
    expected_hash = fnv1a64(expected)
    sock = socks_connect(entry_host, socks_port, PROBE_SINK_HOST, io_timeout)
    started = time.monotonic(); sock.sendall(struct.pack("!4sQ", b"NBP1", len(expected))); sock.sendall(expected)
    response = bytearray()
    while b"NBPROBE OK " not in response:
        chunk = sock.recv(4096)
        if not chunk: break
        response.extend(chunk)
    elapsed = time.monotonic() - started; sock.close()
    match = re.search(rb"NBPROBE OK bytes=(\d+) hash=([0-9a-fA-F]{16})", response)
    received = int(match.group(1)) if match else -1
    received_hash = int(match.group(2), 16) if match else -1
    if received != len(expected) or received_hash != expected_hash:
        raise RuntimeError(f"主动探针完整性失败: sent={len(expected)} recv={received}")
    return {"bytes": size, "elapsed_s": elapsed, "integrity": "ok",
            "origin": "controller"}


def run_load_probe(entry_host: str, socks_port: int, target_mbps: float, duration_s: int,
                   stop_event=None, io_timeout: float = 30) -> dict:
    sock = socks_connect(entry_host, socks_port, PROBE_SINK_HOST, io_timeout)
    chunk = bytes((index * 13 + 29) & 0xff for index in range(16 * 1024))
    target_bytes = int(target_mbps * 1_000_000 / 8.0 * duration_s)
    sock.sendall(struct.pack("!4sQ", b"NBP1", target_bytes))
    started = time.monotonic(); sent = 0
    while sent < target_bytes and not (stop_event and stop_event.is_set()):
        now = time.monotonic()
        allowed = int(target_mbps * 1_000_000 / 8.0 * (now - started + 0.02))
        if sent >= allowed:
            time.sleep(min(0.005, (sent - allowed + 1) * 8.0 / (target_mbps * 1_000_000)))
            continue
        count = min(len(chunk), target_bytes - sent, max(1, allowed - sent))
        sock.sendall(chunk[:count]); sent += count
    sock.shutdown(socket.SHUT_WR)
    response = bytearray()
    while b"NBPROBE OK " not in response:
        data = sock.recv(4096)
        if not data: break
        response.extend(data)
    elapsed = time.monotonic() - started; sock.close()
    match = re.search(rb"NBPROBE OK bytes=(\d+)", response)
    received = int(match.group(1)) if match else -1
    if received != sent:
        raise RuntimeError(f"主动探针计数失败: sent={sent} exit={received} response={response[:100]!r}")
    return {"bytes": sent, "elapsed_s": elapsed,
        "achieved_mbps": sent * 8.0 / max(elapsed, 0.001) / 1_000_000.0,
        "integrity": "cancelled" if stop_event and stop_event.is_set() else "count-ok",
        "origin": "controller"}


def run_downlink_probe(entry_host: str,socks_port: int,target_mbps: float,duration_s: int,
                       io_timeout: float = 30) -> dict:
    target_bytes=int(target_mbps*1_000_000/8.0*duration_s)
    sock=socks_connect(entry_host,socks_port,PROBE_SOURCE_HOST,max(io_timeout,duration_s+15))
    sock.sendall(struct.pack("!4sQ",b"NBP2",target_bytes));started=time.monotonic();received=0
    while received<target_bytes:
        data=sock.recv(min(64*1024,target_bytes-received))
        if not data:break
        expected=bytes((((received+index)*13+29)&0xff) for index in range(len(data)))
        if data!=expected:raise RuntimeError(f"下行探针完整性失败 offset={received}")
        received+=len(data)
    elapsed=time.monotonic()-started;sock.close()
    if received!=target_bytes:raise RuntimeError(f"下行探针计数失败 expected={target_bytes} received={received}")
    return {"bytes":received,"elapsed_s":elapsed,
        "achieved_mbps":received*8.0/max(elapsed,0.001)/1_000_000.0,
        "integrity":"count-ok","origin":"controller"}


def parse_entry_probe_line(raw_line: str) -> dict | None:
    line = raw_line.strip()
    if line.startswith("NBPROBE_PROGRESS "):
        progress = json.loads(line.removeprefix("NBPROBE_PROGRESS "))
        print("probe progress origin=entry-local "
              f"elapsed={progress['elapsed_s']:.1f}s "
              f"throughput={progress['achieved_mbps']:.3f}Mbps", flush=True)
        return None
    if line.startswith("NBPROBE_RESULT "):
        return json.loads(line.removeprefix("NBPROBE_RESULT "))
    return None


def parse_entry_probe_output(lines: list[str]) -> dict:
    result = None
    for line in lines:
        parsed = parse_entry_probe_line(line)
        if parsed is not None:
            result = parsed
    if result is None:
        raise RuntimeError("entry-local probe returned no result")
    return result


def run_entry_local_probe(mode: str, socks_port: int, *, size: int = 256 * 1024,
                          target_mbps: float = 0.0, duration_s: int = 0,
                          io_timeout: float = 30.0) -> dict:
    """Generate qualification traffic on Entry so controller uplink is excluded."""
    if mode not in {"integrity", "load"}:
        raise ValueError("entry-local probe mode must be integrity or load")
    username = os.environ.get("NB_SOCKS_USERNAME", "")
    password = os.environ.get("NB_SOCKS_PASSWORD", "")
    if not username or not password or len(username.encode()) > 255 or len(password.encode()) > 255:
        raise RuntimeError("entry-local probe requires valid SOCKS credentials")
    request = {
        "mode": mode,
        "socks_port": socks_port,
        "username": username,
        "password": password,
        "size": size,
        "target_mbps": target_mbps,
        "duration_s": duration_s,
        "io_timeout": io_timeout,
    }
    connection = deploy.connect("entry")
    command = "python3 -c " + shlex.quote(ENTRY_LOCAL_PROBE_SCRIPT)
    stdin, stdout, stderr = connection.exec_command(
        command, timeout=max(60.0, float(duration_s) + io_timeout + 30.0))
    try:
        stdin.write(json.dumps(request).encode("utf-8"))
        stdin.channel.shutdown_write()
        result = None
        for raw_line in stdout:
            line = raw_line.decode(errors="replace") if isinstance(raw_line, bytes) else raw_line
            parsed = parse_entry_probe_line(line)
            if parsed is not None:
                result = parsed
        error_text = stderr.read().decode(errors="replace")
        exit_status = stdout.channel.recv_exit_status()
    finally:
        connection.close()
    if exit_status != 0:
        raise RuntimeError(f"entry-local probe failed rc={exit_status}: {error_text[-2000:]}")
    if result is None:
        raise RuntimeError("entry-local probe returned no result")
    return result


def remote_log_path(connection, role: str) -> str:
    command = (
        f"bin=$(readlink -f {deploy.INSTANCE_WORK}/nb_node 2>/dev/null); "
        "dir=$(dirname \"$bin\"); "
        f"if test -f \"$dir/cfg/log4c.json\"; then echo {deploy._log_path(role)}; "
        f"else echo \"$dir/logs/nb-{role}.log\"; fi"
    )
    output = deploy.run(connection, command).strip().splitlines()
    path = output[-1].strip() if output else ""
    if not path.startswith("/"):
        raise RuntimeError(f"unable to resolve remote {role} log path")
    return path


def log_offset(role: str) -> int:
    connection = deploy.connect(role)
    try:
        path = remote_log_path(connection, role)
        quoted = shlex.quote(path)
        value = deploy.run(connection,
            f"if test -f {quoted}; then wc -c < {quoted}; else printf '0\\n'; fi").strip()
        lines = [line.strip() for line in value.splitlines() if line.strip()]
        if not lines or not lines[-1].isdigit():
            raise RuntimeError(f"invalid remote {role} log size")
        return int(lines[-1])
    finally:
        connection.close()


def log_since(role: str, offset: int) -> str:
    connection = deploy.connect(role)
    try:
        path = remote_log_path(connection, role)
        quoted = shlex.quote(path)
        start = max(0, offset) + 1
        return deploy.run(connection,
            f"if test -f {quoted}; then tail -c +{start} -- {quoted} 2>/dev/null || true; fi")
    finally:
        connection.close()


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
    log_path = deploy._log_path(source_role)
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


def control_link_sample(records: list[dict]) -> dict:
    links = [record.get("link", {}) for record in records if isinstance(record, dict)]
    return {
        "sent": sum(int(link.get("sent_packets", 0) or 0) for link in links),
        "spurious": sum(int(link.get("spurious_total", 0) or 0) for link in links),
        "rtt": max((float(link.get("rtt_max_us", 0) or 0) / 1000.0 for link in links), default=0.0),
        "jitter": max((float(link.get("jitter_max_us", 0) or 0) / 1000.0 for link in links), default=0.0),
        "loss": max((float(link.get("effective_loss_max_pct", 0) or 0) for link in links), default=0.0),
        "reorder_gap": max((int(link.get("reorder_gap_max", 0) or 0) for link in links), default=0),
        "reorder_ms": max((float(link.get("reorder_delay_max_us", 0) or 0) / 1000.0 for link in links), default=0.0),
        "cwin_kb": max((int(link.get("cwin_max_bytes", 0) or 0) // 1024 for link in links), default=0),
        "block": int(any(int(link.get("blocked_connections", 0) or 0) > 0 for link in links)),
        "mtu": 0,
    }


def collect_control_link_samples(role: str, stop_event: threading.Event,
                                 ready_event: threading.Event, output: list[dict],
                                 errors: list[str], interval_s: float = 5.0) -> None:
    pattern = deploy._control_socket_glob(role)
    prefix = deploy._control_socket_prefix(role)
    socket_pattern = f"/run/{prefix}-[0-9]+\\.ctl"
    script = f"""import glob,json,re,socket
paths=[path for path in sorted(glob.glob({pattern!r})) if re.fullmatch({socket_pattern!r},path)]
if not paths:raise RuntimeError('no matching control sockets')
metrics=[]
for path in paths:
 client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.settimeout(3)
 try:
  client.connect(path);client.sendall(b'metrics\\n');metrics.append(json.loads(client.recv(65536).decode()))
 finally:client.close()
print(json.dumps(metrics,separators=(',',':')))
"""
    connection = None
    try:
        connection = deploy.connect(role)
        while not stop_event.is_set():
            raw = deploy.checked_run(connection, "python3 -c " + shlex.quote(script), tmo=20).strip()
            records = json.loads(raw)
            output.append(control_link_sample(records))
            ready_event.set()
            if stop_event.wait(interval_s):
                break
    except Exception as error:
        errors.append(f"{role}: {type(error).__name__}: {error}")
        ready_event.set()
    finally:
        if connection is not None:
            connection.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ping-samples", type=int, default=30)
    rate = parser.add_mutually_exclusive_group()
    rate.add_argument("--target-mbps", type=float,
        help="直接指定主动探针速率；兼容旧调用，不代表可售套餐")
    rate.add_argument("--package-mbps", type=float,
        help="待开通套餐带宽，探针会按 headroom 倍率做容量准入")
    parser.add_argument("--headroom-ratio", type=float, default=1.25,
        help="套餐容量资格测试余量，默认 1.25")
    parser.add_argument("--active", action="store_true", help="通过真实三跳 QUIC 产生受控负载")
    parser.add_argument("--upstream-mbps",type=float,help="业务上行平均限速验证目标")
    parser.add_argument("--downstream-mbps",type=float,help="业务下行平均限速验证目标")
    parser.add_argument("--duration", type=int, default=90, help="主动负载持续秒数")
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--via-entry-ssh", action="store_true",
                        help="reach the Entry-local SOCKS listener through the pinned SSH connection")
    parser.add_argument("--probe-origin", choices=("entry-local", "controller"),
                        default="entry-local",
                        help="generate qualification traffic on Entry by default")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.via_entry_ssh:
        os.environ["NB_PROBE_VIA_ENTRY_SSH"] = "1"
    if args.headroom_ratio < 1.0 or args.headroom_ratio > 2.0:
        raise SystemExit("--headroom-ratio 必须在 1.0..2.0")
    package_mbps = args.package_mbps
    target_mbps = (package_mbps * args.headroom_ratio if package_mbps is not None
                   else (args.target_mbps if args.target_mbps is not None else 10.0))
    shaping_up=args.upstream_mbps if args.upstream_mbps is not None else (package_mbps or target_mbps)
    shaping_down=args.downstream_mbps if args.downstream_mbps is not None else (package_mbps or target_mbps)
    if package_mbps is not None and (package_mbps < 1.0 or package_mbps > 1000):
        raise SystemExit("--package-mbps 必须在 1..1000")
    if args.ping_samples < 10 or args.ping_samples > 300:
        raise SystemExit("--ping-samples 必须在 10..300")
    if target_mbps <= 0.1 or target_mbps > 2000:
        raise SystemExit("探针目标速率必须在 0.1..2000 Mbps")
    if args.duration < 10 or args.duration > 600:
        raise SystemExit("--duration 必须在 10..600")

    middle = deploy._role_host("middle")
    exit_host = deploy._role_host("exit")
    middle_target = middle.get("private_ip") or middle["host"]
    active_result = None
    active_logs = None
    active_metric_samples = {"entry": [], "middle": []}
    active_metric_errors = []
    if args.active:
        offsets = {role: log_offset(role) for role in ("entry", "middle")}
        sample_stop = threading.Event()
        sample_ready = {role: threading.Event() for role in active_metric_samples}
        sample_threads = [threading.Thread(target=collect_control_link_samples,
            args=(role, sample_stop, sample_ready[role], active_metric_samples[role],
                  active_metric_errors), daemon=True) for role in active_metric_samples]
        for thread in sample_threads:
            thread.start()
        for ready in sample_ready.values():
            ready.wait(30)
        try:
            if args.probe_origin == "entry-local":
                integrity = run_entry_local_probe("integrity", args.socks_port)
                uplink = run_entry_local_probe("load", args.socks_port,
                    target_mbps=shaping_up, duration_s=args.duration)
                downlink = run_entry_local_probe("downlink", args.socks_port,
                    target_mbps=shaping_down, duration_s=args.duration)
            else:
                integrity = run_integrity_probe(deploy._role_host("entry")["host"], args.socks_port)
                uplink = run_load_probe(deploy._role_host("entry")["host"], args.socks_port,
                    shaping_up, args.duration)
                downlink = run_downlink_probe(deploy._role_host("entry")["host"], args.socks_port,
                    shaping_down, args.duration)
        finally:
            sample_stop.set()
            for thread in sample_threads:
                thread.join(25)
        time.sleep(12)
        active_logs = {role: log_since(role, offsets[role]) for role in offsets}
        active_result = {"integrity": integrity, "uplink": uplink, "downlink": downlink}

    entry_segment = collect_segment("entry", middle_target, args.ping_samples, target_mbps)
    middle_segment = collect_segment("middle", exit_host["host"], args.ping_samples, target_mbps)
    if active_logs is not None:
        for role, segment in (("entry", entry_segment), ("middle", middle_segment)):
            samples = active_metric_samples[role]
            summary = summarize_linkq(samples if samples else parse_linkq(active_logs[role]))
            segment["quic"] = summary
            segment["candidate"] = recommend(summary,
                deploy.LAB.get("transport", {}).get(role, {}), target_mbps,
                segment.get("mtu"))

    result = {
        "schema_version": 2,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "line": f"{deploy._role_host('entry')['name']}-{middle['name']}-{exit_host['name']}",
        "fixed_exit": exit_host["name"],
        "probe_mode": "active-quic" if args.active else "passive-runtime",
        "status": "candidate",
        "baseline_hosts_sha256": hashlib.sha256(deploy.LAB_FILE.read_bytes()).hexdigest(),
        "baseline_profile_sha256": hashlib.sha256(deploy.LINE_PROFILE.read_bytes()).hexdigest() if deploy.LINE_PROFILE.is_file() else None,
        "active_probe": active_result,
        "metric_collection_errors": active_metric_errors,
        "service_package": {
            "committed_mbps": package_mbps,
            "upstream_mbps":shaping_up,"downstream_mbps":shaping_down,
            "qualification_mbps": target_mbps,
            "headroom_ratio": args.headroom_ratio if package_mbps is not None else None,
        },
        "admission": evaluate_admission(active_result, shaping_up, shaping_down) if args.active else {
            "status": "not-evaluated",
            "reasons": ["active-quic-required"],
            "target_mbps": min(shaping_up,shaping_down),
        },
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
