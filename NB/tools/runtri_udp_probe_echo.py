#!/usr/bin/env python3
"""Exercise NB's native UDP probe echo through a live runtri SOCKS entry."""

from __future__ import annotations

import argparse
import ipaddress
import json
import socket
import struct
import sys
import time


TARGET_HOST = "nb-probe-echo.internal"
TARGET_PORT = 9
PAYLOAD = bytes((index * 73 + 19) & 0xFF for index in range(2501))


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError(f"short SOCKS response {len(data)}/{size}")
        data.extend(chunk)
    return bytes(data)


def recv_address(sock: socket.socket) -> tuple[str, int]:
    head = recv_exact(sock, 4)
    if head[:3] != b"\x05\x00\x00":
        raise RuntimeError(f"SOCKS request rejected: {head!r}")
    atyp = head[3]
    if atyp == 1:
        host = socket.inet_ntoa(recv_exact(sock, 4))
    elif atyp == 3:
        host = recv_exact(sock, recv_exact(sock, 1)[0]).decode("ascii")
    elif atyp == 4:
        host = socket.inet_ntop(socket.AF_INET6, recv_exact(sock, 16))
    else:
        raise RuntimeError(f"unsupported SOCKS ATYP={atyp}")
    return host, struct.unpack("!H", recv_exact(sock, 2))[0]


def control_health(path: str) -> dict:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(path)
        client.sendall(b"health\n")
        chunks = []
        while True:
            chunk = client.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
        return json.loads(b"".join(chunks))
    finally:
        client.close()


def udp_sessions(path: str) -> int:
    return int(control_health(path)["flow_sessions"]["udp"])


def exactly_zero(values: list[int]) -> bool:
    return all(value == 0 for value in values)


def require_stable_zero(controls: list[str]) -> None:
    first = [udp_sessions(path) for path in controls]
    second = [udp_sessions(path) for path in controls]
    if not exactly_zero(first) or not exactly_zero(second):
        raise RuntimeError(f"unstable UDP session baseline: first={first}, second={second}")


def wait_for_reclaim(controls: list[str]) -> None:
    deadline = time.monotonic() + 10
    consecutive_zero = 0
    while time.monotonic() < deadline:
        current = [udp_sessions(path) for path in controls]
        if exactly_zero(current):
            consecutive_zero += 1
            if consecutive_zero == 2:
                return
        else:
            consecutive_zero = 0
        time.sleep(0.1)
    current = [udp_sessions(path) for path in controls]
    raise RuntimeError(f"UDP child was not reclaimed: current={current}")


def read_password(stream: object) -> bytes:
    line = stream.readline()
    if not isinstance(line, str):
        raise RuntimeError("password stdin is not text")
    password = line.rstrip("\r\n").encode("ascii")
    if not password or len(password) > 255:
        raise RuntimeError("invalid SOCKS password from stdin")
    return password


def numeric_ipv4_relay(host: str, port: int) -> tuple[str, int]:
    try:
        address = ipaddress.ip_address(host)
        if address.version == 4:
            return str(address), port
    except ValueError:
        pass
    for family, _kind, _protocol, _canonname, endpoint in socket.getaddrinfo(
        host, port, socket.AF_INET, socket.SOCK_DGRAM
    ):
        if family == socket.AF_INET:
            return endpoint[0], endpoint[1]
    raise RuntimeError(f"SOCKS UDP relay has no numeric IPv4 address: {host!r}")


def recv_expected_peer(udp: socket.socket, expected: tuple[str, int], attempts: int) -> bytes:
    bad_peer: tuple[object, ...] | None = None
    for _ in range(attempts):
        packet, peer = udp.recvfrom(65535)
        peer_endpoint = (peer[0], peer[1])
        if peer_endpoint != expected:
            bad_peer = peer
            continue
        if bad_peer is not None:
            raise RuntimeError(f"unexpected UDP peer {bad_peer!r} before expected relay {expected!r}")
        return packet
    raise RuntimeError(f"unexpected UDP peer {bad_peer!r}; expected relay {expected!r}")


def associate(entry_host: str, entry_port: int, username: bytes, password: bytes) -> tuple[socket.socket, tuple[str, int]]:
    control = socket.create_connection((entry_host, entry_port), timeout=10)
    control.settimeout(10)
    control.sendall(b"\x05\x01\x02")
    if recv_exact(control, 2) != b"\x05\x02":
        raise RuntimeError("SOCKS username/password authentication was not selected")
    control.sendall(b"\x01" + bytes([len(username)]) + username + bytes([len(password)]) + password)
    if recv_exact(control, 2) != b"\x01\x00":
        raise RuntimeError("SOCKS authentication failed")
    control.sendall(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00")
    return control, recv_address(control)


def encode_socks_udp(host: str, port: int, payload: bytes) -> bytes:
    encoded = host.encode("ascii")
    return b"\x00\x00\x00\x03" + bytes([len(encoded)]) + encoded + struct.pack("!H", port) + payload


def decode_socks_udp(packet: bytes) -> tuple[str, int, bytes]:
    if len(packet) < 5 or packet[:3] != b"\x00\x00\x00" or packet[3] != 3:
        raise RuntimeError(f"expected SOCKS UDP ATYP=3 response, got {packet[:4]!r}")
    host_length = packet[4]
    offset = 5 + host_length
    if len(packet) < offset + 2:
        raise RuntimeError("truncated SOCKS UDP domain response")
    return packet[5:offset].decode("ascii"), struct.unpack("!H", packet[offset:offset + 2])[0], packet[offset + 2:]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--entry-host", default="127.0.0.1")
    parser.add_argument("--entry-port", type=int, required=True)
    parser.add_argument("--username", required=True)
    parser.add_argument("--password-stdin", action="store_true")
    parser.add_argument("--control", action="append", required=True)
    args = parser.parse_args()
    username = args.username.encode("ascii")
    if not args.password_stdin:
        parser.error("--password-stdin is required")
    password = read_password(sys.stdin)
    if not username or not password or len(username) > 255 or len(password) > 255:
        raise RuntimeError("invalid SOCKS credentials")
    require_stable_zero(args.control)
    control, relay = associate(args.entry_host, args.entry_port, username, password)
    relay_host, relay_port = relay
    if relay_host in ("0.0.0.0", "::"):
        relay_host = args.entry_host
    relay_endpoint = numeric_ipv4_relay(relay_host, relay_port)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(10)
    try:
        udp.sendto(encode_socks_udp(TARGET_HOST, TARGET_PORT, PAYLOAD), relay_endpoint)
        host, port, payload = decode_socks_udp(recv_expected_peer(udp, relay_endpoint, 3))
        if (host, port, payload) != (TARGET_HOST, TARGET_PORT, PAYLOAD):
            raise RuntimeError(f"UDP echo mismatch host={host!r} port={port} bytes={len(payload)}")
    finally:
        udp.close()
        control.close()
    wait_for_reclaim(args.control)
    print("RESULT PASS: native UDP probe echo ATYP=3 2501-byte payload and child reclaim")


if __name__ == "__main__":
    main()
