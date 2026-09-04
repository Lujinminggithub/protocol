#!/usr/bin/env python3
"""本地校验候选媒体/Exit 修复证据，及可选的隔离 UDP 回显探针。"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import ipaddress
import json
import pathlib
import shlex
import socket
import sys
import time
import threading
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
INSTANCE = "gz2-hk2-kz-00002_1"
WHITELIST_PATH = f"/etc/NB/instances/{INSTANCE}/whitelist.conf"
LOOPBACK_RULE = b"ip 127.0.0.1/32\n"
BURST_MIN_RECEIVED_BYTES = 742500
SUSTAINED_MIN_KBPS = 4500
SUSTAINED_MAX_KBPS = 5000


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
        old = _mapping(before.get(key), f"queue_pressure_before.{key}")
        new = _mapping(after.get(key), f"queue_pressure_after.{key}")
        worker = next(item for item in workers if _worker_key(item) == key)
        for field in ("release_id", "boot_id"):
            if (_text(old.get(field), f"queue_pressure_before.{key}.{field}") !=
                    _text(new.get(field), f"queue_pressure_after.{key}.{field}") or
                    _text(old.get(field), f"queue_pressure_before.{key}.{field}") !=
                    _text(worker.get(field), f"worker.{field}")):
                raise ValueError(f"证据 queue_pressure {key} 身份漂移: {field}")
        old_generation = _integer(old.get("transport_generation"),
                                  f"queue_pressure_before.{key}.transport_generation")
        new_generation = _integer(new.get("transport_generation"),
                                  f"queue_pressure_after.{key}.transport_generation")
        if old_generation != new_generation or old_generation != _integer(worker.get("generation"), "worker.generation"):
            raise ValueError(f"证据 queue_pressure {key} transport generation 漂移")
        old_pressure = _integer(old.get("queue_pressure_dropped"), f"queue_pressure_before.{key}.queue_pressure_dropped")
        new_pressure = _integer(new.get("queue_pressure_dropped"), f"queue_pressure_after.{key}.queue_pressure_dropped")
        if new_pressure < old_pressure:
            raise ValueError(f"证据 queue_pressure_dropped 计数回退: {key}")
        if new_pressure != old_pressure:
            raise ValueError(f"证据 queue_pressure_dropped 增量非零: {key}")
        delta += new_pressure - old_pressure
    return delta


def _parse_timestamp(value: Any, label: str) -> dt.datetime:
    text = _text(value, label)
    try:
        return dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError(f"私网证据 {label} 时间格式非法") from error


def _validate_private_probe(evidence: dict[str, Any], workers: list[dict[str, Any]],
                            release: str, deployment: str) -> dict[str, Any]:
    probe = _mapping(evidence.get("private_dns_probe"), "private_dns_probe")
    host = _text(probe.get("host"), "private_dns_probe.host")
    address = _text(probe.get("address"), "private_dns_probe.address")
    if not _is_tiktok_row_private(host, address):
        raise ValueError("私网证据必须是 *.tiktok-row.net 的 RFC1918 地址")
    elapsed_ms = _integer(probe.get("elapsed_ms"), "private_dns_probe.elapsed_ms")
    if elapsed_ms >= FAST_PRIVATE_FAILURE_MAX_MS:
        raise ValueError("私网证据没有快速失败")
    if probe.get("result") != "private-unreachable":
        raise ValueError("私网证据 result 不正确")
    if probe.get("close_reason") != "target-private-unreachable":
        raise ValueError("私网证据 close reason 不正确")
    before = _mapping(probe.get("exit_connectivity_before"), "private_dns_probe.before")
    after = _mapping(probe.get("exit_connectivity_after"), "private_dns_probe.after")
    for field in ("worker_key", "release_id", "deployment", "boot_id"):
        if _text(before.get(field), f"private_dns_probe.before.{field}") != _text(
                after.get(field), f"private_dns_probe.after.{field}"):
            raise ValueError("私网证据 before/after 身份不一致")
    worker_key = _text(before.get("worker_key"), "private_dns_probe.before.worker_key")
    matched = next((worker for worker in workers if _worker_key(worker) == worker_key), None)
    if matched is None or matched.get("role") != "exit":
        raise ValueError("私网证据未绑定 Exit worker")
    if (_text(before.get("release_id"), "private_dns_probe.before.release_id") != release or
            _text(before.get("deployment"), "private_dns_probe.before.deployment") != deployment or
            _text(before.get("boot_id"), "private_dns_probe.before.boot_id") !=
            _text(matched.get("boot_id"), "worker.boot_id")):
        raise ValueError("私网证据身份与 Exit worker 不一致")
    if _parse_timestamp(after.get("sampled_at_utc"), "private_dns_probe.after.sampled_at_utc") < _parse_timestamp(
            before.get("sampled_at_utc"), "private_dns_probe.before.sampled_at_utc"):
        raise ValueError("私网证据采样时间窗倒退")
    old = _integer(before.get("dns_private_rejected"), "private_dns_probe.before.dns_private_rejected")
    new = _integer(after.get("dns_private_rejected"), "private_dns_probe.after.dns_private_rejected")
    if new <= old:
        raise ValueError("私网证据没有可见 dns_private_rejected 增量")
    _integer(after.get("dns_failures"), "private_dns_probe.after.dns_failures")
    return {"worker_key": worker_key, "host": host, "address": address, "elapsed_ms": elapsed_ms,
            "dns_private_rejected_delta": new - old}


def _validate_synthetic(evidence: dict[str, Any]) -> dict[str, Any] | None:
    raw = evidence.get("synthetic_validation")
    if raw is None:
        return None
    synthetic = _mapping(raw, "synthetic_validation")
    burst = _mapping(synthetic.get("burst"), "synthetic_validation.burst")
    sustained = _mapping(synthetic.get("sustained"), "synthetic_validation.sustained")
    if _integer(burst.get("planned_bps"), "synthetic_validation.burst.planned_bps") != 12000000:
        raise ValueError("合成证据 burst 计划速率错误")
    if _integer(burst.get("planned_duration_ms"), "synthetic_validation.burst.planned_duration_ms") != 500:
        raise ValueError("合成证据 burst 计划时长错误")
    if _integer(burst.get("sent_bytes"), "synthetic_validation.burst.sent_bytes") < 750000:
        raise ValueError("合成证据 burst 发送字节不足")
    if _integer(burst.get("received_bytes"), "synthetic_validation.burst.received_bytes") < BURST_MIN_RECEIVED_BYTES:
        raise ValueError("合成证据 burst 回显字节不足")
    elapsed_ms = _integer(burst.get("elapsed_ms"), "synthetic_validation.burst.elapsed_ms")
    if not 500 <= elapsed_ms <= 1000:
        raise ValueError("合成证据 burst 发送窗口必须为 500-1000ms")
    duration_s = sustained.get("duration_s")
    average_kbps = sustained.get("average_kbps")
    if (isinstance(duration_s, bool) or not isinstance(duration_s, (int, float)) or
            duration_s < 90 or isinstance(average_kbps, bool) or
            not isinstance(average_kbps, (int, float)) or
            not SUSTAINED_MIN_KBPS <= average_kbps <= SUSTAINED_MAX_KBPS):
        raise ValueError("合成证据持续吞吐必须为 4500-5000Kbps 且至少 90 秒")
    return {"burst": dict(burst), "sustained": dict(sustained)}


def _phone_validation(evidence: dict[str, Any]) -> dict[str, Any] | None:
    raw = evidence.get("phone_validation")
    if raw is None:
        return None
    phone = _mapping(raw, "phone_validation")
    if any(field not in phone for field in ("click_at_utc", "enter_at_utc", "state_timeline", "viewer_events")):
        return None
    _parse_timestamp(phone.get("click_at_utc"), "phone_validation.click_at_utc")
    _parse_timestamp(phone.get("enter_at_utc"), "phone_validation.enter_at_utc")
    timeline = phone.get("state_timeline")
    events = phone.get("viewer_events")
    if not isinstance(timeline, list):
        raise ValueError("手机证据缺少 red/yellow/green 状态时间线")
    states = set()
    for index, item in enumerate(timeline):
        state = _mapping(item, f"phone_validation.state_timeline[{index}]")
        name = _text(state.get("state"), f"phone_validation.state_timeline[{index}].state")
        _parse_timestamp(state.get("at_utc"), f"phone_validation.state_timeline[{index}].at_utc")
        states.add(name)
    if not {"red", "yellow", "green"}.issubset(states):
        raise ValueError("手机证据缺少 red/yellow/green 状态时间线")
    if not isinstance(events, list) or len(events) < 2:
        raise ValueError("手机证据缺少两次观看端事件")
    for index, item in enumerate(events):
        event = _mapping(item, f"phone_validation.viewer_events[{index}]")
        _text(event.get("event"), f"phone_validation.viewer_events[{index}].event")
        _parse_timestamp(event.get("at_utc"), f"phone_validation.viewer_events[{index}].at_utc")
        _integer(event.get("duration_ms"), f"phone_validation.viewer_events[{index}].duration_ms")
    return dict(phone)


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
    private_probe = _validate_private_probe(document, workers, release, deployment)
    synthetic = _validate_synthetic(document)
    phone = _phone_validation(document)
    result = {
        "release": release,
        "deployment": deployment,
        "generation": 2,
        "workers": len(workers),
        "signal_direct": False,
        "tenant": dict(TENANT_EXPECTED),
        "queue_pressure_delta": queue_delta,
        "private_dns_probe": private_probe,
    }
    if synthetic is None or phone is None:
        result["status"] = "pending_phone_validation"
        result["missing"] = [name for name, value in (("synthetic_validation", synthetic),
                            ("phone_validation", phone)) if value is None]
        return result
    result["status"] = "passed"
    result["synthetic_validation"] = synthetic
    result["phone_validation"] = phone
    return result


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


class WhitelistLease(Protocol):
    def install(self) -> None: ...
    def restore(self) -> None: ...


class NoopWhitelistLease:
    def install(self) -> None:
        pass

    def restore(self) -> None:
        pass


class WhitelistOperations(Protocol):
    def read(self, role: str) -> bytes: ...
    def stage(self, role: str, data: bytes, path: str) -> None: ...
    def publish(self, role: str, source: str) -> None: ...
    def verify_reload(self, role: str, expected_sha256: str) -> None: ...
    def cleanup(self, role: str, paths: tuple[str, ...]) -> None: ...


def _append_loopback_rule(original: bytes) -> bytes:
    try:
        text = original.decode("ascii")
    except UnicodeDecodeError as error:
        raise RuntimeError("临时白名单不是 ASCII，拒绝修改") from error
    lines = text.splitlines()
    if "ip 127.0.0.1/32" in {line.strip() for line in lines}:
        return ("\n".join(lines) + "\n").encode("ascii")
    return ("\n".join(lines) + "\n" + LOOPBACK_RULE.decode("ascii")).encode("ascii")


class TemporaryWhitelistLease:
    """Atomically stage, activate, validate and restore the loopback-only probe rule."""
    def __init__(self, operations: WhitelistOperations) -> None:
        self.operations = operations
        self.originals: dict[str, bytes] = {}
        self.published: list[str] = []
        self.paths: dict[str, tuple[str, str]] = {}

    def install(self) -> None:
        self.originals = {role: self.operations.read(role) for role in ("entry", "exit")}
        if self.originals["entry"] != self.originals["exit"]:
            raise RuntimeError("Entry/Exit 白名单基线不一致，拒绝临时放行")
        candidate = _append_loopback_rule(self.originals["entry"])
        stamp = hashlib.sha256(candidate).hexdigest()[:16]
        for role in ("entry", "exit"):
            root = f"/tmp/nb-media-exit-probe-{stamp}-{role}"
            self.paths[role] = (root + ".backup", root + ".candidate")
            backup, staged = self.paths[role]
            self.operations.stage(role, self.originals[role], backup)
            self.operations.stage(role, candidate, staged)
        for role in ("entry", "exit"):
            self.operations.publish(role, self.paths[role][1])
            self.published.append(role)
        expected = hashlib.sha256(candidate).hexdigest()
        for role in ("entry", "exit"):
            self.operations.verify_reload(role, expected)

    def restore(self) -> None:
        errors = []
        for role in reversed(self.published):
            try:
                backup, _ = self.paths[role]
                self.operations.publish(role, backup)
                self.operations.verify_reload(role, hashlib.sha256(self.originals[role]).hexdigest())
            except Exception as error:
                errors.append(f"{role}:恢复:{type(error).__name__}")
        for role, paths in self.paths.items():
            try:
                self.operations.cleanup(role, paths)
            except Exception as error:
                errors.append(f"{role}:清理:{type(error).__name__}")
        self.published = []
        if errors:
            raise RuntimeError("临时白名单恢复或清理失败: " + ",".join(errors))


class RemoteWhitelistOperations:
    """Lazy deployment adapter; no connection is created by local evidence validation."""
    def __init__(self) -> None:
        tools = ROOT / "tools"
        if str(tools) not in sys.path:
            sys.path.insert(0, str(tools))
        import deploy  # pylint: disable=import-outside-toplevel
        import nb_p1_control  # pylint: disable=import-outside-toplevel
        self.deploy = deploy
        self.control = nb_p1_control

    def read(self, role: str) -> bytes:
        return self.control._remote_read(role, WHITELIST_PATH)

    def stage(self, role: str, data: bytes, path: str) -> None:
        self.control._remote_push(role, data, path)

    def publish(self, role: str, source: str) -> None:
        self.control._publish(role, source, WHITELIST_PATH)

    def verify_reload(self, role: str, expected_sha256: str) -> None:
        connection = self.deploy.connect(role)
        try:
            command = (
                f"test $(sha256sum {shlex.quote(WHITELIST_PATH)} | awk '{{print $1}}') = "
                f"{shlex.quote(expected_sha256)} && sleep 6 && "
                f"test $(sha256sum {shlex.quote(WHITELIST_PATH)} | awk '{{print $1}}') = "
                f"{shlex.quote(expected_sha256)}"
            )
            self.deploy.checked_run(connection, command, tmo=20)
        finally:
            connection.close()
        health = self.control.invoke_role(role, "health")
        if len(health) != 2 or any(item.get("status") != "ok" for item in health):
            raise RuntimeError(f"{role} 白名单 reload 后控制端状态不健康")

    def cleanup(self, role: str, paths: tuple[str, ...]) -> None:
        connection = self.deploy.connect(role)
        try:
            self.deploy.checked_run(connection, "rm -f " + " ".join(shlex.quote(path) for path in paths), tmo=15)
        finally:
            connection.close()


def run_isolated_probe(request: dict[str, Any], adapter: IsolatedProbeAdapter,
                       whitelist: WhitelistLease | None = None) -> dict[str, Any]:
    """Keep temporary echo and whitelist state inside one outer try/finally gate."""
    parsed = parse_runner_request(request)
    lease = whitelist or NoopWhitelistLease()
    echo = None
    result = None
    operation_error = None
    try:
        lease.install()
        echo = adapter.open_echo()
        result = adapter.run_traffic(parsed, echo)
    except Exception as error:
        operation_error = error
    finally:
        cleanup_errors = []
        if echo is not None:
            try:
                adapter.close_echo(echo)
            except Exception as error:
                cleanup_errors.append(f"echo:{type(error).__name__}")
        try:
            lease.restore()
        except Exception as error:
            cleanup_errors.append(f"whitelist:{type(error).__name__}")
        if cleanup_errors:
            raise RuntimeError("临时探针清理失败: " + ",".join(cleanup_errors)) from operation_error
    if operation_error is not None:
        raise operation_error
    assert result is not None
    return result


@dataclass
class _EchoState:
    connection: Any
    pid: int
    control: socket.socket
    udp: socket.socket
    relay: tuple[str, int]


def strict_kill_echo(connection: Any, pid: int, checked_run: Any) -> None:
    """Require the temporary Exit echo process group to be terminated successfully."""
    if type(pid) is not int or pid <= 0:
        raise RuntimeError("临时 echo pid 非法")
    checked_run(connection, f"kill -- -{pid}", tmo=15)


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
            try:
                strict_kill_echo(connection, pid, udp_e2e_probe.deploy.checked_run)
            finally:
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
                    strict_kill_echo(echo.connection, echo.pid, udp_e2e_probe.deploy.checked_run)
                finally:
                    echo.connection.close()

    @staticmethod
    def _measure(echo: _EchoState, rate_bps: int, duration_s: float, kind: str) -> dict[str, Any]:
        """Count sequenced, de-duplicated echo payloads while paced sends are in progress."""
        from udp_e2e_probe import decode_datagram, encode_datagram  # pylint: disable=import-outside-toplevel

        target_bytes = int(rate_bps * duration_s // 8)
        sent_bytes = 0
        sequence = 0
        sent_sizes: dict[int, int] = {}
        received: dict[int, int] = {}
        receiver_errors: list[str] = []
        receive_lock = threading.Lock()
        stop = threading.Event()

        def receive_loop() -> None:
            while not stop.is_set():
                try:
                    data, _ = echo.udp.recvfrom(65535)
                except BlockingIOError:
                    time.sleep(0.001)
                    continue
                except OSError as error:
                    receiver_errors.append(type(error).__name__)
                    return
                try:
                    host, port, payload = decode_datagram(data)
                    if host != "127.0.0.1" or port != ECHO_PORT or len(payload) < 8:
                        continue
                    item = int.from_bytes(payload[:8], "big")
                    with receive_lock:
                        if item in sent_sizes and item not in received and len(payload) == sent_sizes[item]:
                            received[item] = len(payload)
                except (RuntimeError, ValueError):
                    continue

        receiver = threading.Thread(target=receive_loop, name="nb-media-echo-recv", daemon=True)
        start = time.monotonic()
        deadline = start + duration_s
        receiver.start()
        try:
            while sent_bytes < target_bytes:
                packet_size = min(1200, target_bytes - sent_bytes)
                due = start + ((sent_bytes + packet_size) * 8 / rate_bps)
                now = time.monotonic()
                if now < due:
                    time.sleep(min(0.005, due - now))
                    continue
                payload = sequence.to_bytes(8, "big") + bytes(packet_size - 8)
                datagram = encode_datagram("127.0.0.1", ECHO_PORT, payload)
                sent_sizes[sequence] = len(payload)
                try:
                    accepted = echo.udp.sendto(datagram, echo.relay)
                except BlockingIOError:
                    sent_sizes.pop(sequence)
                    time.sleep(0.001)
                    continue
                if accepted != len(datagram):
                    raise RuntimeError("隔离 UDP 回显探针只接受了部分数据报")
                sent_bytes += len(payload)
                sequence += 1
            send_elapsed_s = time.monotonic() - start
            receive_deadline = time.monotonic() + 0.75
            while time.monotonic() < receive_deadline:
                with receive_lock:
                    if len(received) == len(sent_sizes):
                        break
                time.sleep(0.002)
        finally:
            stop.set()
            receiver.join(2)
        with receive_lock:
            received_bytes = sum(received.values())
        if receiver.is_alive() or receiver_errors or sent_bytes != target_bytes:
            raise RuntimeError("隔离 UDP 回显探针接收线程或发送证据异常")
        if kind == "media_burst":
            if not 0.5 <= send_elapsed_s <= 1.0 or received_bytes < BURST_MIN_RECEIVED_BYTES:
                raise RuntimeError("隔离 UDP 突发未在 500-1000ms 内取得 99% 回显")
        elif send_elapsed_s < 90 or not SUSTAINED_MIN_KBPS <= received_bytes * 8 / send_elapsed_s / 1000 <= SUSTAINED_MAX_KBPS:
            raise RuntimeError("隔离 UDP 持续探针吞吐未处于 4500-5000Kbps")
        if kind == "media_burst":
            return {"planned_bps": rate_bps, "planned_duration_ms": 500,
                    "sent_bytes": sent_bytes, "received_bytes": received_bytes,
                    "elapsed_ms": int(send_elapsed_s * 1000)}
        return {"average_kbps": received_bytes * 8 / send_elapsed_s / 1000,
                "duration_s": send_elapsed_s, "sent_bytes": sent_bytes,
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
            result = run_isolated_probe(json.loads(args.media_burst_runner), RealIsolatedEchoAdapter(),
                                        TemporaryWhitelistLease(RemoteWhitelistOperations()))
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        return 0
    except (ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(json.dumps({"status": "failed", "error": str(error)}, ensure_ascii=False,
                         separators=(",", ":")))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
