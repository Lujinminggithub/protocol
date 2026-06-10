#!/usr/bin/env python3
import argparse
import socket
import threading
import traceback


def log(message: str) -> None:
    print(message, flush=True)


def relay(a: socket.socket, b: socket.socket) -> None:
    try:
        while True:
            data = a.recv(4096)
            if not data:
                break
            b.sendall(data)
    finally:
        try:
            a.close()
        finally:
            b.close()


def handle(conn: socket.socket) -> None:
    try:
        log("http.stage accept")
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = conn.recv(4096)
            if not chunk:
                return
            request += chunk
        first = request.split(b"\r\n", 1)[0].decode("utf-8", errors="ignore")
        parts = first.split()
        if len(parts) < 3 or parts[0].upper() != "CONNECT":
            log(f"http.error invalid-request line={first}")
            conn.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n\r\n")
            return
        host, port_text = parts[1].rsplit(":", 1)
        log(f"http.stage connect.begin host={host} port={port_text}")
        upstream = socket.create_connection((host, int(port_text)), timeout=10)
        log(f"http.stage connect.ok host={host} port={port_text}")
        conn.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        log("http.stage tunnel.established")
        threading.Thread(target=relay, args=(conn, upstream), daemon=True).start()
        relay(upstream, conn)
    except Exception as exc:
        log(f"http.error connect.failed {exc}")
        log(traceback.format_exc())
        try:
            conn.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
        except Exception:
            pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", args.port))
    server.listen(32)
    log(f"http.listen {args.port}")
    while True:
        conn, _ = server.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    raise SystemExit(main())
