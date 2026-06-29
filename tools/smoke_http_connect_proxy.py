#!/usr/bin/env python3
import argparse
import socket
import threading
import time
import traceback


def log(message: str) -> None:
    print(message, flush=True)


def close_quietly(sock: socket.socket) -> None:
    try:
        sock.close()
    except Exception:
        pass


def shutdown_write_quietly(sock: socket.socket) -> None:
    try:
        sock.shutdown(socket.SHUT_WR)
    except Exception:
        pass


def relay_one_way(src: socket.socket, dst: socket.socket, label: str) -> None:
    try:
        while True:
            data = src.recv(4096)
            if not data:
                break
            dst.sendall(data)
    except OSError as exc:
        log(f"http.debug relay.{label}.closed {exc}")
    finally:
        shutdown_write_quietly(dst)


def handle(conn: socket.socket) -> None:
    upstream = None
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
        upstream = None
        last_exc = None
        for attempt in range(5):
            try:
                upstream = socket.create_connection((host, int(port_text)), timeout=10)
                break
            except Exception as exc:
                last_exc = exc
                log(f"http.warn connect.retry attempt={attempt + 1} err={exc}")
                time.sleep(1)
        if upstream is None:
            raise last_exc if last_exc is not None else ConnectionError("connect failed")
        log(f"http.stage connect.ok host={host} port={port_text}")
        conn.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        log("http.stage tunnel.established")
        client_to_upstream = threading.Thread(
            target=relay_one_way,
            args=(conn, upstream, "client_to_upstream"),
            daemon=True,
        )
        client_to_upstream.start()
        relay_one_way(upstream, conn, "upstream_to_client")
        client_to_upstream.join(timeout=2)
    except Exception as exc:
        log(f"http.error connect.failed {exc}")
        log(traceback.format_exc())
        try:
            conn.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
        except Exception:
            pass
    finally:
        if upstream is not None:
            close_quietly(upstream)
        close_quietly(conn)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(("0.0.0.0", args.port))
        actual_port = server.getsockname()[1]
        server.listen(32)
        log(f"http.listen {actual_port}")
    except Exception as exc:
        log(f"http.fatal bind_or_listen_failed {exc}")
        log(traceback.format_exc())
        raise
    while True:
        try:
            conn, addr = server.accept()
            log(f"http.stage accepted from={addr[0]}:{addr[1]}")
            threading.Thread(target=handle, args=(conn,), daemon=True).start()
        except Exception as exc:
            log(f"http.error accept.failed {exc}")
            log(traceback.format_exc())
            time.sleep(1)


if __name__ == "__main__":
    raise SystemExit(main())
