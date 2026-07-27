#!/usr/bin/env python3
"""Real three-hop SOCKS5 UDP ASSOCIATE integrity probe."""
from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import json
import os
import pathlib
import shlex
import socket
import time

import deploy
import nb_observe
from netem_matrix import atomic_report, checked, validate_cluster


def recv_exact(sock: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise RuntimeError("SOCKS control connection closed")
        data.extend(chunk)
    return bytes(data)


def read_address(sock: socket.socket, atyp: int) -> tuple[str, int]:
    if atyp == 1:
        host = str(ipaddress.ip_address(recv_exact(sock, 4)))
    elif atyp == 4:
        host = str(ipaddress.ip_address(recv_exact(sock, 16)))
    elif atyp == 3:
        host = recv_exact(sock, recv_exact(sock, 1)[0]).decode("ascii")
    else:
        raise RuntimeError(f"unsupported SOCKS ATYP={atyp}")
    return host, int.from_bytes(recv_exact(sock, 2), "big")


def udp_associate(entry_host: str, socks_port: int) -> tuple[socket.socket, tuple[str, int]]:
    user = os.environ.get("NB_SOCKS_USERNAME", "").encode()
    password = os.environ.get("NB_SOCKS_PASSWORD", "").encode()
    if not user or not password or len(user) > 255 or len(password) > 255:
        raise RuntimeError("SOCKS probe credentials are missing")
    control = socket.create_connection((entry_host, socks_port), timeout=15)
    control.settimeout(15)
    control.sendall(b"\x05\x01\x02")
    if recv_exact(control, 2) != b"\x05\x02":
        raise RuntimeError("SOCKS username/password method rejected")
    control.sendall(b"\x01" + bytes([len(user)]) + user + bytes([len(password)]) + password)
    if recv_exact(control, 2) != b"\x01\x00":
        raise RuntimeError("SOCKS authentication rejected")
    control.sendall(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00")
    header = recv_exact(control, 4)
    if header[:3] != b"\x05\x00\x00":
        raise RuntimeError(f"UDP ASSOCIATE rejected code={header[1]}")
    host, port = read_address(control, header[3])
    if host in ("0.0.0.0", "::"):
        host = entry_host
    return control, (host, port)


def encode_datagram(host: str, port: int, payload: bytes) -> bytes:
    address = ipaddress.ip_address(host)
    atyp = 1 if address.version == 4 else 4
    return b"\x00\x00\x00" + bytes([atyp]) + address.packed + port.to_bytes(2, "big") + payload


def decode_datagram(data: bytes) -> tuple[str, int, bytes]:
    if len(data) < 4 or data[:3] != b"\x00\x00\x00":
        raise RuntimeError("malformed SOCKS UDP response")
    atyp = data[3]
    address_length = 4 if atyp == 1 else 16 if atyp == 4 else 0
    if not address_length or len(data) < 4 + address_length + 2:
        raise RuntimeError("unsupported or truncated SOCKS UDP response")
    offset = 4 + address_length
    host = str(ipaddress.ip_address(data[4:offset]))
    port = int.from_bytes(data[offset:offset + 2], "big")
    return host, port, data[offset + 2:]


def start_echo(port: int) -> tuple[object, int]:
    connection = deploy.connect("exit")
    checked(connection, "command -v python3 >/dev/null")
    checked(connection,
        f"if ss -H -lun 'sport = :{port}' | grep -q .; then false; else true; fi")
    program = ("import socket,time\n"
        "s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n"
        f"s.bind(('127.0.0.1',{port}))\n"
        "s.settimeout(.5); end=time.time()+120\n"
        "while time.time()<end:\n"
        " try:\n"
        "  data,peer=s.recvfrom(65535); s.sendto(data,peer)\n"
        " except socket.timeout: pass\n")
    raw = checked(connection,
        f"nohup setsid python3 -u -c {shlex.quote(program)} >/dev/null 2>&1 & echo $!")
    pid = int(raw.splitlines()[-1])
    time.sleep(1)
    checked(connection, f"kill -0 {pid}")
    return connection, pid


def run(args) -> dict:
    snapshot = nb_observe.collect()
    validate_cluster(snapshot)
    workers = snapshot["workers"]
    deployment = next(iter({str(w["health"]["release_id"]) for w in workers}))
    profile = next(iter({f"{w['health']['line_profile']}:{w['health']['line_profile_schema']}"
                         for w in workers}))
    report = {
        "schema_version": 1,
        "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "deployment": deployment,
        "profile": profile,
        "status": "running",
        "cases": [],
    }
    atomic_report(args.output, report)
    connection = None
    pid = 0
    control = None
    udp = None
    try:
        connection, pid = start_echo(args.echo_port)
        control, relay = udp_associate(deploy._role_host("entry")["host"], args.socks_port)
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for size in args.sizes:
            payload = bytes((index * 31 + size) & 0xff for index in range(size))
            started = time.monotonic()
            passed = False
            host = ""
            port = 0
            attempts = 0
            for attempts in range(1, args.attempts + 1):
                udp.sendto(encode_datagram("127.0.0.1", args.echo_port, payload), relay)
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    udp.settimeout(max(0.1, deadline - time.monotonic()))
                    try:
                        data, _ = udp.recvfrom(65535)
                    except TimeoutError:
                        break
                    host, port, echoed = decode_datagram(data)
                    if echoed == payload and port == args.echo_port:
                        passed = True
                        break
                if passed:
                    break
            case = {"payload_bytes": size, "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
                    "attempts": attempts, "target": f"{host}:{port}" if host else None,
                    "integrity": "ok" if passed else "timeout", "passed": passed}
            report["cases"].append(case)
        report["status"] = "passed" if all(case["passed"] for case in report["cases"]) else "failed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if udp is not None:
            udp.close()
        if control is not None:
            control.close()
        if connection is not None:
            if pid:
                deploy.run(connection, f"kill -- -{pid} 2>/dev/null || true")
            connection.close()
    report["finished_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    atomic_report(args.output, report)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Run three-hop SOCKS5 UDP integrity cases")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--sizes", type=lambda value: [int(x) for x in value.split(",")],
                        default=[1, 999, 1000, 1001, 60000])
    parser.add_argument("--echo-port", type=int, default=50020)
    parser.add_argument("--socks-port", type=int, default=1080)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--attempts", type=int, default=3)
    args = parser.parse_args()
    if not args.sizes or any(size < 1 or size > 65000 for size in args.sizes):
        parser.error("sizes must be in 1..65000")
    if not 1 <= args.attempts <= 5 or not 1 <= args.timeout <= 60:
        parser.error("attempts must be 1..5 and timeout 1..60")
    report = run(args)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
