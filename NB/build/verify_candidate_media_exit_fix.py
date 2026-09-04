#!/usr/bin/env python3
"""本地校验候选媒体/Exit 修复证据，及可选的隔离 UDP 回显探针。"""
from __future__ import annotations

import argparse
import ipaddress
import json
import pathlib
import socket
import sys
import time
from dataclasses import dataclass
from typing import Any, Protocol


ROOT = pathlib.Path(__file__).resolve().parents[1]
ROLES = ("entry", "middle", "exit")
EXIT_CONNECTIVITY_FIELDS = (
    "dns_requests",
    "dns_failures",
    "dns_private_rejected",
    "dns_latency_max_us",
    "target_connect_timeouts",
    "target_connect_latency_max_us",
)
TENANT_EXPECTED = {
    "rate_up_kbps": 5000,
    "rate_down_kbps": 5000,
    "burst_up_bytes": 2500000,
    "burst_down_bytes": 625000,
}
FAST_PRIVATE_FAILURE_MAX_MS = 1000
ECHO_PORT = 50020


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"证据 {label} 必须是对象")
    return value


def _integer(value: Any, label: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ValueError(f"证据 {label} 必须是大于等于 {minimum} 的整数")
    return value


def _text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"证据 {label} 必须是非空字符串")
    return value.strip()


def _worker_key(worker: dict[str, Any]) -> str:
    return f"{_text(worker.get('role'), 'worker.role')}:{_text(worker.get('worker'), 'worker.worker')}"


def _is_tiktok_row_private(host: str, address: str) -> bool:
    try:
        parsed = ipaddress.ip_address(address)
    except ValueError as error:
        raise ValueError("私网证据地址非法") from error
    normalized = host.rstrip(".").lower()
    if parsed.version != 4 or not normalized.endswith(".tiktok-row.net"):
        return False
    if normalized == "tiktok-row.net":
        return False
    value = int(parsed)
    return (value >> 24 == 10 or
            value >> 20 == ((172 << 4) | 1) or
            value >> 16 == ((192 << 8) | 168))


def _validate_workers(evidence: dict[str, Any], deployment: str) -> tuple[list[dict[str, Any]], str]:
    raw_workers = evidence.get("workers")
    if not isinstance(raw_workers, list) or len(raw_workers) != 6:
        raise ValueError("证据 workers 必须恰好包含六个 worker")
    workers: list[dict[str, Any]] = []
    seen = set()
    releases = set()
    for raw in raw_workers:
        worker = _mapping(raw, "worker")
        role = _text(worker.get("role"), "worker.role")
        if role not in ROLES:
            raise ValueError("证据 worker.role 非法")
        key = _worker_key(worker)
        if key in seen:
            raise ValueError("证据 worker 重复")
        seen.add(key)
        if worker.get("status") != "ok":
            raise ValueError("证据 worker 状态不是 ok")
        if _text(worker.get("deployment"), "worker.deployment") != deployment:
            raise ValueError("证据 worker deployment 不一致")
        if _integer(worker.get("generation"), "worker.generation") != 2:
            raise ValueError("证据 worker generation 不是 2")
        releases.add(_text(worker.get("release_id"), "worker.release_id"))
        metrics = _mapping(worker.get("metrics"), "worker.metrics")
        udp_errors = _mapping(metrics.get("udp_errors"), "worker.metrics.udp_errors")
        _integer(udp_errors.get("queue_pressure_dropped"), "queue_pressure_dropped")
        if role == "exit":
            connectivity = _mapping(metrics.get("exit_connectivity"), "exit_connectivity")
            for field in EXIT_CONNECTIVITY_FIELDS:
                _integer(connectivity.get(field), f"exit_connectivity.{field}")
        workers.append(worker)
    if {worker["role"] for worker in workers} != set(ROLES):
        raise ValueError("证据 worker 角色不完整")
    for role in ROLES:
        if sum(worker["role"] == role for worker in workers) != 2:
            raise ValueError(f"证据 {role} worker 数量不是两个")
    if len(releases) != 1:
        raise ValueError("证据 worker release 不一致")
    return workers, next(iter(releases))


def _validate_tenant(evidence: dict[str, Any]) -> None:
    tenant = _mapping(evidence.get("tenant"), "tenant")
    if set(tenant) != {"entry", "exit"}:
        raise ValueError("证据 tenant 必须包含 entry 与 exit")
    for role in ("entry", "exit"):
        values = _mapping(tenant.get(role), f"tenant.{role}")
        if values != TENANT_EXPECTED:
            raise ValueError(f"证据 tenant.{role} 不是 5000/5000 与 2500000/625000")


def _validate_queue_pressure(evidence: dict[str, Any], workers: list[dict[str, Any]]) -> int:
    before = _mapping(evidence.get("queue_pressure_before"), "queue_pressure_before")
    after = _mapping(evidence.get("queue_pressure_after"), "queue_pressure_after")
    expected_keys = {_worker_key(worker) for worker in workers}
    if set(before) != expected_keys or set(after) != expected_keys:
        raise ValueError("证据 queue_pressure 必须覆盖同一组六个 worker")
    delta = 0
    for key in sorted(expected_keys):
        old = _integer(before.get(key), f"queue_pressure_before.{key}")
        new = _integer(after.get(key), f"queue_pressure_after.{key}")
        if new != old:
            raise ValueError(f"证据 queue_pressure_dropped 增量非零: {key}")
        delta += new - old
    return delta


def _validate_private_probe(evidence: dict[str, Any]) -> dict[str, Any]:
    probe = _mapping(evidence.get("private_dns_probe"), "private_dns_probe")
    host = _text(probe.get("host"), "private_dns_probe.host")
    address = _text(probe.get("address"), "private_dns_probe.address")
    if not _is_tiktok_row_private(host, address):
        raise ValueError("私网证据必须是 *.tiktok-row.net 的 RFC1918 地址")
    elapsed_ms = _integer(probe.get("elapsed_ms"), "private_dns_probe.elapsed_ms")
    if elapsed_ms > FAST_PRIVATE_FAILURE_MAX_MS:
        raise ValueError("私网证据没有快速失败")
    if probe.get("result") != "private-unreachable":
        raise ValueError("私网证据 result 不正确")
    if probe.get("close_reason") != "target-private-unreachable":
        raise ValueError("私网证据 close reason 不正确")
    before = _mapping(probe.get("exit_connectivity_before"), "private_dns_probe.before")
    after = _mapping(probe.get("exit_connectivity_after"), "private_dns_probe.after")
    old = _integer(before.get("dns_private_rejected"), "private_dns_probe.before.dns_private_rejected")
    new = _integer(after.get("dns_private_rejected"), "private_dns_probe.after.dns_private_rejected")
    if new <= old:
        raise ValueError("私网证据没有可见 dns_private_rejected 增量")
    _integer(after.get("dns_failures"), "private_dns_probe.after.dns_failures")
    return {"host": host, "address": address, "elapsed_ms": elapsed_ms,
            "dns_private_rejected_delta": new - old}


def validate_acceptance(evidence: dict[str, Any]) -> dict[str, Any]:
    """Fail closed on incomplete local readback evidence; never contacts a server."""
    document = _mapping(evidence, "根对象")
    controlplane = _mapping(document.get("controlplane"), "controlplane")
    deployment = _text(controlplane.get("deployment"), "controlplane.deployment")
    if _integer(controlplane.get("generation"), "controlplane.generation") != 2:
        raise ValueError("证据 controlplane generation 不是 2")
    if controlplane.get("signal_direct") is not False:
        raise ValueError("证据 signal_direct 必须为 false")
    workers, release = _validate_workers(document, deployment)
    _validate_tenant(document)
    queue_delta = _validate_queue_pressure(document, workers)
    private_probe = _validate_private_probe(document)
    return {
        "status": "passed",
        "release": release,
        "deployment": deployment,
        "generation": 2,
        "workers": len(workers),
        "signal_direct": False,
        "tenant": dict(TENANT_EXPECTED),
        "queue_pressure_delta": queue_delta,
        "private_dns_probe": private_probe,
    }


def parse_runner_request(request: Any) -> dict[str, int | str]:
    """Accept only the two fixed, bounded requests made by deploy_candidate_media_burst."""
    value = _mapping(request, "探针请求")
    kind = value.get("kind")
    if kind == "media_burst":
        if value != {"kind": "media_burst", "rate_mbps": 12, "duration_ms": 500}:
            raise ValueError("媒体突发探针请求必须固定为 12Mbps/500ms")
        return {"kind": kind, "rate_mbps": 12, "duration_ms": 500}
    if kind == "sustained_probe":
        if value != {"kind": "sustained_probe", "duration_s": 90, "maximum_kbps": 5000}:
            raise ValueError("持续探针请求必须固定为 5Mbps/90s")
        return {"kind": kind, "duration_s": 90, "maximum_kbps": 5000}
    raise ValueError("探针请求 kind 非法")


class IsolatedProbeAdapter(Protocol):
    def open_echo(self) -> Any: ...
    def run_traffic(self, request: dict[str, int | str], echo: Any) -> dict[str, Any]: ...
    def close_echo(self, echo: Any) -> None: ...


def run_isolated_probe(request: dict[str, Any], adapter: IsolatedProbeAdapter) -> dict[str, Any]:
    """Run a bounded probe and always remove its temporary Exit echo process."""
    parsed = parse_runner_request(request)
    echo = adapter.open_echo()
    try:
        return adapter.run_traffic(parsed, echo)
    finally:
        adapter.close_echo(echo)


@dataclass
class _EchoState:
    connection: Any
    pid: int
    control: socket.socket
    udp: socket.socket
    relay: tuple[str, int]


class RealIsolatedEchoAdapter:
    """Uses the existing three-hop SOCKS probe's loopback-only temporary echo service."""
    def open_echo(self) -> _EchoState:
        tools = ROOT / "tools"
        if str(tools) not in sys.path:
            sys.path.insert(0, str(tools))
        import deploy  # pylint: disable=import-outside-toplevel
        import udp_e2e_probe  # pylint: disable=import-outside-toplevel

        connection, pid = udp_e2e_probe.start_echo(ECHO_PORT)
        control = None
        udp = None
        try:
            control, relay = udp_e2e_probe.udp_associate(deploy._role_host("entry")["host"], 1080)
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.setblocking(False)
            return _EchoState(connection, pid, control, udp, relay)
        except Exception:
            if udp is not None:
                udp.close()
            if control is not None:
                control.close()
            udp_e2e_probe.deploy.run(connection, f"kill -- -{pid} 2>/dev/null || true")
            connection.close()
            raise

    def run_traffic(self, request: dict[str, int | str], echo: _EchoState) -> dict[str, Any]:
        if request["kind"] == "media_burst":
            return self._measure(echo, 12_000_000, 0.5, "media_burst")
        return self._measure(echo, 5_000_000, 90.0, "sustained_probe")

    def close_echo(self, echo: _EchoState) -> None:
        try:
            echo.udp.close()
        finally:
            try:
                echo.control.close()
            finally:
                try:
                    import udp_e2e_probe  # pylint: disable=import-outside-toplevel
                    udp_e2e_probe.deploy.run(echo.connection, f"kill -- -{echo.pid} 2>/dev/null || true")
                finally:
                    echo.connection.close()

    @staticmethod
    def _measure(echo: _EchoState, rate_bps: int, duration_s: float, kind: str) -> dict[str, Any]:
        """Count only payload bytes actually accepted by UDP and echoed back over SOCKS."""
        from udp_e2e_probe import decode_datagram, encode_datagram  # pylint: disable=import-outside-toplevel

        target_bytes = int(rate_bps * duration_s // 8)
        sent_bytes = 0
        received_bytes = 0
        sequence = 0
        start = time.monotonic()
        deadline = start + duration_s
        while sent_bytes < target_bytes:
            packet_size = min(1200, target_bytes - sent_bytes)
            due = start + ((sent_bytes + packet_size) * 8 / rate_bps)
            now = time.monotonic()
            if now < due:
                time.sleep(min(0.01, due - now))
                continue
            payload = sequence.to_bytes(8, "big") + bytes(packet_size - 8)
            try:
                accepted = echo.udp.sendto(encode_datagram("127.0.0.1", ECHO_PORT, payload), echo.relay)
            except BlockingIOError:
                time.sleep(0.001)
                continue
            if accepted <= 0:
                raise RuntimeError("隔离 UDP 回显探针未接受数据")
            sent_bytes += len(payload)
            sequence += 1
        receive_deadline = max(deadline, time.monotonic()) + 5.0
        while time.monotonic() < receive_deadline:
            try:
                data, _ = echo.udp.recvfrom(65535)
            except BlockingIOError:
                time.sleep(0.002)
                continue
            host, port, payload = decode_datagram(data)
            if host == "127.0.0.1" and port == ECHO_PORT and len(payload) >= 8:
                received_bytes += len(payload)
        elapsed_s = time.monotonic() - start
        if sent_bytes != target_bytes or received_bytes == 0 or elapsed_s < duration_s:
            raise RuntimeError("隔离 UDP 回显探针未取得完整的真实传输证据")
        if kind == "media_burst":
            return {"planned_bps": rate_bps, "planned_duration_ms": 500,
                    "sent_bytes": sent_bytes, "received_bytes": received_bytes,
                    "elapsed_ms": int(elapsed_s * 1000)}
        return {"average_kbps": received_bytes * 8 / elapsed_s / 1000,
                "duration_s": elapsed_s, "sent_bytes": sent_bytes,
                "received_bytes": received_bytes}


def _load_evidence(path: pathlib.Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError("本地证据文件无法解析") from error
    return _mapping(document, "根对象")


def main() -> int:
    parser = argparse.ArgumentParser(description="本地验证候选媒体/Exit 修复证据")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--evidence", type=pathlib.Path, help="只读取本地 JSON 证据并 fail-closed 验证")
    group.add_argument("--media-burst-runner", metavar="REQUEST_JSON",
                       help="作为 NB_MEDIA_BURST_RUNNER 运行隔离三跳 UDP echo 探针")
    args = parser.parse_args()
    try:
        if args.evidence is not None:
            result = validate_acceptance(_load_evidence(args.evidence))
        else:
            result = run_isolated_probe(json.loads(args.media_burst_runner), RealIsolatedEchoAdapter())
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        return 0
    except (ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(json.dumps({"status": "failed", "error": str(error)}, ensure_ascii=False,
                         separators=(",", ":")))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
