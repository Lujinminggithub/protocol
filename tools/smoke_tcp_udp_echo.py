#!/usr/bin/env python3
import argparse
import socket
import threading
import time


def run_tcp(port: int) -> None:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", port))
    actual_port = server.getsockname()[1]
    server.listen(16)
    print(f"echo.tcp.listen {actual_port}", flush=True)
    while True:
        conn, _ = server.accept()
        threading.Thread(target=handle_tcp, args=(conn,), daemon=True).start()


def handle_tcp(conn: socket.socket) -> None:
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            conn.sendall(data)
    finally:
        conn.close()


def run_udp(port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    actual_port = sock.getsockname()[1]
    print(f"echo.udp.listen {actual_port}", flush=True)
    while True:
        data, addr = sock.recvfrom(65535)
        sock.sendto(data, addr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tcp-port", type=int, required=True)
    parser.add_argument("--udp-port", type=int, default=0)
    args = parser.parse_args()
    threading.Thread(target=run_tcp, args=(args.tcp_port,), daemon=True).start()
    if args.udp_port > 0:
        run_udp(args.udp_port)
        return 0
    while True:
        time.sleep(3600)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
