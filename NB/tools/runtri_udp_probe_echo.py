#!/usr/bin/env python3
"""Exercise NB's native UDP probe echo through a live runtri SOCKS entry."""

from __future__ import annotations

import argparse
import json
import socket
import struct
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


def wait_for_reclaim(controls: list[str], baseline: list[int]) -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if all(udp_sessions(path) <= before for path, before in zip(controls, baseline)):
            return
        time.sleep(0.1)
    current = [udp_sessions(path) for path in controls]
    raise RuntimeError(f"UDP child was not reclaimed: baseline={baseline}, current={current}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--entry-host", default="127.0.0.1")
    parser.add_argument("--entry-port", type=int, required=True)
    parser.add_argument("--username", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--control", action="append", required=True)
    args = parser.parse_args()
    username = args.username.encode("ascii")
    password = args.password.encode("ascii")
    if not username or not password or len(username) > 255 or len(password) > 255:
        raise RuntimeError("invalid SOCKS credentials")
    baseline = [udp_sessions(path) for path in args.control]
    control, relay = associate(args.entry_host, args.entry_port, username, password)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(10)
    try:
        relay_host, relay_port = relay
        if relay_host in ("0.0.0.0", "::"):
            relay_host = args.entry_host
        udp.sendto(encode_socks_udp(TARGET_HOST, TARGET_PORT, PAYLOAD), (relay_host, relay_port))
        host, port, payload = decode_socks_udp(udp.recvfrom(65535)[0])
        if (host, port, payload) != (TARGET_HOST, TARGET_PORT, PAYLOAD):
            raise RuntimeError(f"UDP echo mismatch host={host!r} port={port} bytes={len(payload)}")
    finally:
        udp.close()
        control.close()
    wait_for_reclaim(args.control, baseline)
    print("RESULT PASS: native UDP probe echo ATYP=3 2501-byte payload and child reclaim")


if __name__ == "__main__":
    main()
