#!/usr/bin/env python3
"""Exercise two isolated lines inside three long-lived NB shard processes."""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time

import security_setup


def free_ports(count: int) -> list[int]:
    reservations: list[socket.socket] = []
    ports: list[int] = []
    try:
        first, width = 12000, 18000
        candidate = first + (os.getpid() * 17) % width
        for _ in range(width):
            tcp, udp = socket.socket(), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                tcp.bind(("127.0.0.1", candidate));udp.bind(("127.0.0.1", candidate))
            except OSError:
                tcp.close();udp.close()
            else:
                reservations.extend((tcp, udp));ports.append(candidate)
                if len(ports) == count:
                    return ports
            candidate = first + (candidate - first + 1) % width
        raise RuntimeError("no dual TCP/UDP test port range is available")
    finally:
        for reservation in reservations:
            reservation.close()


def echo_client(client: socket.socket) -> None:
    with client:
        client.settimeout(5)
        try:
            while data := client.recv(65536):
                client.sendall(data)
        except OSError:
            pass


def echo_server(listener: socket.socket, stop: threading.Event) -> None:
    listener.settimeout(0.2)
    while not stop.is_set():
        try:
            client, _ = listener.accept()
        except TimeoutError:
            continue
        threading.Thread(target=echo_client, args=(client,), daemon=True).start()


def write_config(path: pathlib.Path, instance: str, control: pathlib.Path,
                 arguments: list[str]) -> None:
    lines = ["schema=1", f"instance_id={instance}", f"control_path={control}"]
    lines.extend(f"arg={value}" for value in arguments)
    lines.extend(("env.NB_INSECURE_TEST_MODE=1", "env.NB_WORKER_LANE_PORTS=off",
                  f"env.NB_CONTROL_PREFIX=nb-{instance}"))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    path.chmod(0o600)


def install_line(root: pathlib.Path, security: pathlib.Path, name: str, socks: int,
                 middle: int, exit_port: int, target_port: int,
                 queue_limit: int = 2 * 1024 * 1024, sessions: int = 16) -> list[pathlib.Path]:
    whitelist = root / f"{name}.whitelist"
    whitelist.write_text(f"ip 127.0.0.1/32\nport {target_port}\n", encoding="utf-8")
    whitelist.chmod(0o600)
    controls = [root / f"{name}-{role}.ctl" for role in ("entry", "middle", "exit")]
    common = ["--max-sessions", str(sessions), "--max-queue-bytes", str(queue_limit)]
    def tls(role: str) -> list[str]:
        return ["-c", str(security / f"{role}.pem"), "-k", str(security / f"{role}.key"),
                "-a", str(security / "ca.pem")]
    write_config(root / "exit" / f"{name}.conf", f"{name}-exit", controls[2],
        ["-r", "exit", "-p", str(exit_port), "-W", str(whitelist), *tls("exit"), *common])
    write_config(root / "middle" / f"{name}.conf", f"{name}-middle", controls[1],
        ["-r", "middle", "-p", str(middle), *tls("middle"), *common])
    write_config(root / "entry" / f"{name}.conf", f"{name}-entry", controls[0],
        ["-r", "entry", "-l", str(socks), "-n", "127.0.0.1", "-N", str(middle),
         "-S", "-W", str(whitelist), "-M", f"H:127.0.0.1:{exit_port}",
         *tls("entry"), *common])
    return controls


def wait_paths(paths: list[pathlib.Path], present: bool = True, timeout: float = 15) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if all(path.exists() == present for path in paths):
            return
        time.sleep(0.1)
    raise RuntimeError(f"control sockets did not become {'ready' if present else 'absent'}")


def socks_roundtrip(port: int, target_port: int, payload: bytes, allowed: bool) -> None:
    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        client.sendall(b"\x05\x01\x00")
        if client.recv(2) != b"\x05\x00":
            raise RuntimeError("SOCKS method negotiation failed")
        request = b"\x05\x01\x00\x01" + socket.inet_aton("127.0.0.1") + struct.pack("!H", target_port)
        client.sendall(request);reply = client.recv(10)
        if len(reply) < 2 or (reply[1] == 0) != allowed:
            raise RuntimeError(f"SOCKS isolation verdict mismatch allowed={allowed} reply={reply.hex()}")
        if allowed:
            client.sendall(payload)
            if client.recv(len(payload)) != payload:
                raise RuntimeError("payload integrity failed")


def wait_socks_ready(port: int, target_port: int, label: str,
                     consecutive: int = 1, timeout: float = 30) -> None:
    deadline = time.monotonic() + timeout
    passed = 0
    while time.monotonic() < deadline:
        try:
            socks_roundtrip(port, target_port,
                f"{label}-{passed}".encode("ascii"), True)
            passed += 1
            if passed >= consecutive:
                return
        except (OSError, RuntimeError):
            passed = 0
            time.sleep(0.2)
    raise RuntimeError(f"{label} did not converge")


def open_socks_tunnel(port: int, target_port: int) -> socket.socket:
    client = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        client.sendall(b"\x05\x01\x00")
        if client.recv(2) != b"\x05\x00":
            raise RuntimeError("SOCKS method negotiation failed")
        request = b"\x05\x01\x00\x01" + socket.inet_aton("127.0.0.1") + struct.pack("!H", target_port)
        client.sendall(request)
        reply = client.recv(10)
        if len(reply) < 2 or reply[1] != 0:
            raise RuntimeError("SOCKS tunnel open failed")
        return client
    except Exception:
        client.close()
        raise


def wait_open_socks_tunnel(port: int, target_port: int, timeout: float = 30) -> socket.socket:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return open_socks_tunnel(port, target_port)
        except (OSError, RuntimeError):
            time.sleep(0.2)
    raise RuntimeError("SOCKS tunnel did not converge")


def assert_socks_rejected(port: int, target_port: int) -> None:
    try:
        client = open_socks_tunnel(port, target_port)
    except (OSError, RuntimeError):
        return
    client.close()
    raise RuntimeError("session limit accepted an excess SOCKS stream")


def assert_cross_worker_port_conflict(binary: pathlib.Path, root: pathlib.Path) -> None:
    cases = (("entry", 18000, 18000), ("middle", 18100, 18099))
    for role, first_port, second_port in cases:
        registry = root / f"port-conflict-{role}" / role
        for worker, (name, port) in enumerate((("line-a", first_port), ("line-b", second_port))):
            directory = registry / str(worker);directory.mkdir(parents=True, mode=0o700)
            option = "-l" if role == "entry" else "-p"
            write_config(directory / f"{name}.conf", f"{name}-{role}",
                root / f"conflict-{role}-{worker}-{name}.ctl",
                ["-r", role, option, str(port)])
        result = subprocess.run([str(binary), "--shard-dir", str(registry / "0")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5, check=False)
        if result.returncode == 0:
            raise RuntimeError(f"cross-worker {role} listen port conflict was accepted")


def control_query(path: pathlib.Path, command: str = "health") -> dict:
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(3);client.connect(str(path));client.sendall(command.encode() + b"\n")
        return json.loads(client.recv(65536))


def wait_sessions(path: pathlib.Path, expected: int, timeout: float = 15) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if int(control_query(path)["resources"]["sessions"]) == expected:
                return
        except (OSError, KeyError, ValueError):
            pass
        time.sleep(0.1)
    raise RuntimeError(f"session count did not reach {expected}")


def wait_line_reload(paths: list[pathlib.Path], previous_boots: list[int],
                     sessions: int, queue_limit: int, timeout: float = 15) -> list[dict]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            current = [control_query(path) for path in paths]
            if (all(item["boot_id"] != previous_boots[index] for index, item in enumerate(current)) and
                    all(item["status"] == "ok" for item in current) and
                    all(item["resources"]["sessions_limit"] == sessions and
                        item["resources"]["queue_limit_bytes"] == queue_limit for item in current)):
                return current
        except (OSError, KeyError, ValueError):
            pass
        time.sleep(0.1)
    raise RuntimeError("line roles did not converge after hot reload")


def assert_no_test_loop(paths: list[pathlib.Path]) -> None:
    for path in paths:
        health = control_query(path)
        if int(health.get("logical_rx", {}).get("fin", 0)) > 32:
            raise RuntimeError(f"logical FIN amplification on {path.name}")
        metrics = control_query(path, "metrics")
        transferred = metrics.get("bytes", {})
        if max(int(transferred.get("c2s", 0)), int(transferred.get("s2c", 0))) > 1024 * 1024:
            raise RuntimeError(f"unexpected payload amplification on {path.name}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit("binary is unavailable")
    root = pathlib.Path(tempfile.mkdtemp(prefix="nb-shard-runtime-"))
    stop = threading.Event();listeners: list[socket.socket] = [];processes: list[subprocess.Popen] = []
    diagnostic_controls: list[pathlib.Path] = []
    try:
        assert_cross_worker_port_conflict(binary, root)
        for role in ("entry", "middle", "exit"):
            (root / role).mkdir(mode=0o700)
        security = root / "security"
        security_setup.generate(security, "shard-test", "shard-test-password")
        target_a, target_b, socks_a, middle_a, exit_a, socks_b, middle_b, exit_b = free_ports(8)
        for port in (target_a, target_b):
            listener = socket.socket();listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", port));listener.listen(16);listeners.append(listener)
            threading.Thread(target=echo_server, args=(listener, stop), daemon=True).start()
        controls_a = install_line(root, security, "line-a", socks_a, middle_a, exit_a, target_a)
        for role in ("exit", "middle", "entry"):
            log = (root / f"{role}.log").open("w", encoding="utf-8")
            launcher = ["stdbuf", "-oL", "-eL"] if shutil.which("stdbuf") else []
            processes.append(subprocess.Popen([*launcher, str(binary), "--shard-dir", str(root / role)],
                stdout=log, stderr=subprocess.STDOUT, text=True))
            log.close()
        wait_paths(controls_a)
        diagnostic_controls.extend(controls_a)
        wait_socks_ready(socks_a, target_a, "line-a-initial", consecutive=2)
        socks_roundtrip(socks_a, target_b, b"cross-a", False)
        controls_b = install_line(root, security, "line-b", socks_b, middle_b, exit_b, target_b)
        wait_paths(controls_b)
        diagnostic_controls.extend(controls_b)
        wait_socks_ready(socks_b, target_b, "line-b-initial", consecutive=2)
        socks_roundtrip(socks_b, target_a, b"cross-b", False)
        socks_roundtrip(socks_a, target_a, b"line-a-still-live", True)
        assert_no_test_loop([*controls_a, *controls_b])
        health_a_all = [control_query(path) for path in controls_a]
        health_b_all = [control_query(path) for path in controls_b]
        health_a, health_b = health_a_all[0], health_b_all[0]
        assert health_a["resources"]["queue_limit_bytes"] == 2 * 1024 * 1024
        assert health_b["resources"]["sessions_limit"] == 16
        install_line(root, security, "line-b", socks_b, middle_b, exit_b, target_b,
                     queue_limit=3 * 1024 * 1024, sessions=1)
        changed_b_all = wait_line_reload(controls_b,
            [item["boot_id"] for item in health_b_all], 1, 3 * 1024 * 1024)
        if [control_query(path)["boot_id"] for path in controls_a] != [item["boot_id"] for item in health_a_all]:
            raise RuntimeError("line-b reload restarted line-a")
        socks_roundtrip(socks_a, target_a, b"line-a-after-b-reload", True)
        held_b = wait_open_socks_tunnel(socks_b, target_b)
        wait_sessions(controls_b[0], 1)
        try:
            assert_socks_rejected(socks_b, target_b)
            socks_roundtrip(socks_a, target_a, b"line-a-while-b-session-capped", True)
        finally:
            held_b.close()

        capped_boots = [item["boot_id"] for item in changed_b_all]
        install_line(root, security, "line-b", socks_b, middle_b, exit_b, target_b,
                     queue_limit=3 * 1024 * 1024, sessions=16)
        wait_line_reload(controls_b, capped_boots, 16, 3 * 1024 * 1024)
        wait_socks_ready(socks_b, target_b, "line-b-after-cap-reload", consecutive=3)

        boot_a = [control_query(path)["boot_id"] for path in controls_a]
        boot_b = [control_query(path)["boot_id"] for path in controls_b]
        invalid = root / "entry" / "invalid.conf"
        invalid.write_text("schema=1\nunknown=reject-me\n", encoding="utf-8")
        invalid.chmod(0o600)
        time.sleep(2.5)
        if [control_query(path)["boot_id"] for path in controls_a] != boot_a:
            raise RuntimeError("invalid config restarted line-a")
        if [control_query(path)["boot_id"] for path in controls_b] != boot_b:
            raise RuntimeError("invalid config restarted line-b")
        socks_roundtrip(socks_a, target_a, b"line-a-during-invalid-config", True)
        socks_roundtrip(socks_b, target_b, b"line-b-during-invalid-config", True)
        invalid.unlink()

        install_line(root, security, "line-b", socks_a, middle_b, exit_b, target_b,
                     queue_limit=3 * 1024 * 1024, sessions=16)
        time.sleep(2.5)
        if [control_query(path)["boot_id"] for path in controls_a] != boot_a:
            raise RuntimeError("line-b port conflict restarted line-a")
        if [control_query(path)["boot_id"] for path in controls_b] != boot_b:
            raise RuntimeError("line-b port conflict replaced the last known-good line-b")
        socks_roundtrip(socks_a, target_a, b"line-a-during-b-conflict", True)
        socks_roundtrip(socks_b, target_b, b"line-b-during-own-conflict", True)
        install_line(root, security, "line-b", socks_b, middle_b, exit_b, target_b,
                     queue_limit=3 * 1024 * 1024, sessions=16)
        wait_paths([controls_b[0]])
        socks_roundtrip(socks_b, target_b, b"line-b-after-conflict-recovery", True)
        for role in ("entry", "middle", "exit"):
            (root / role / "line-a.conf").unlink()
        wait_paths(controls_a, present=False)
        socks_roundtrip(socks_b, target_b, b"line-b-after-remove", True)
        assert_no_test_loop(controls_b)
        for process in processes:
            children = subprocess.run(["pgrep", "-P", str(process.pid), "nb_node"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            if children.returncode == 0:
                raise RuntimeError("shard spawned an unexpected nb_node child process")
        print("RESULT PASS")
    except Exception:
        for path in diagnostic_controls:
            try:
                print(f"--- control {path.name} ---")
                print(json.dumps(control_query(path), sort_keys=True))
                print(json.dumps(control_query(path, "metrics"), sort_keys=True))
            except (OSError, ValueError) as error:
                print(f"--- control {path.name} unavailable: {error} ---")
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=5)
        processes.clear()
        for role in ("exit", "middle", "entry"):
            log_path = root / f"{role}.log"
            if log_path.exists():
                print(f"--- {role} shard log ---")
                print(log_path.read_text(encoding="utf-8", errors="replace")[-12000:])
        raise
    finally:
        stop.set()
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=5)
        for listener in listeners:
            listener.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
