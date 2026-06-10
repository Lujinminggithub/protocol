#!/usr/bin/env python3
import argparse
import socket
import struct
import threading
import traceback


def log(message: str) -> None:
    print(message, flush=True)


def read_exact(conn: socket.socket, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("unexpected EOF")
        data += chunk
    return data


def parse_addr(conn: socket.socket):
    atyp = read_exact(conn, 1)[0]
    if atyp == 1:
        host = socket.inet_ntoa(read_exact(conn, 4))
    elif atyp == 3:
        ln = read_exact(conn, 1)[0]
        host = read_exact(conn, ln).decode()
    else:
        raise ValueError("unsupported atyp")
    port = struct.unpack("!H", read_exact(conn, 2))[0]
    return atyp, host, port


def pack_addr(host: str, port: int) -> bytes:
    try:
        ip = socket.inet_aton(host)
        return b"\x01" + ip + struct.pack("!H", port)
    except OSError:
        host_b = host.encode()
        return b"\x03" + bytes([len(host_b)]) + host_b + struct.pack("!H", port)


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


def handle_tcp(conn: socket.socket, username: str, password: str, advertise_host: str) -> None:
    udp_client = {"addr": None}
    udp_sock = None
    try:
        log("socks5.stage tcp.accept")
        ver, nmethods = read_exact(conn, 2)
        methods = read_exact(conn, nmethods)
        if username and password:
            log("socks5.stage auth.userpass")
            conn.sendall(b"\x05\x02")
            ver = read_exact(conn, 1)[0]
            ulen = read_exact(conn, 1)[0]
            user = read_exact(conn, ulen).decode()
            plen = read_exact(conn, 1)[0]
            pwd = read_exact(conn, plen).decode()
            if user != username or pwd != password:
                log("socks5.error auth.failed")
                conn.sendall(b"\x01\x01")
                return
            conn.sendall(b"\x01\x00")
            log("socks5.stage auth.ok")
        else:
            conn.sendall(b"\x05\x00")
        ver, cmd, rsv = read_exact(conn, 3)
        atyp, host, port = parse_addr(conn)
        log(f"socks5.stage request cmd={cmd} host={host} port={port}")
        if cmd == 1:
            upstream = socket.create_connection((host, port), timeout=10)
            conn.sendall(b"\x05\x00\x00" + pack_addr("0.0.0.0", 0))
            log(f"socks5.stage tcp.connect.ok host={host} port={port}")
            threading.Thread(target=relay, args=(conn, upstream), daemon=True).start()
            relay(upstream, conn)
            return
        if cmd == 3:
            udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp_sock.bind(("0.0.0.0", 0))
            _, bind_port = udp_sock.getsockname()
            reply_host = advertise_host or conn.getsockname()[0]
            conn.sendall(b"\x05\x00\x00" + pack_addr(reply_host, bind_port))
            log(f"socks5.stage udp.associate.ok bind={reply_host}:{bind_port}")
            threading.Thread(target=udp_loop, args=(udp_sock, udp_client), daemon=True).start()
            while conn.recv(1024):
                pass
            return
        log(f"socks5.error unsupported.cmd={cmd}")
        conn.sendall(b"\x05\x07\x00" + pack_addr("0.0.0.0", 0))
    except Exception as exc:
        log(f"socks5.exception {exc}")
        log(traceback.format_exc())
    finally:
        try:
            conn.close()
        except Exception:
            pass
        if udp_sock is not None:
            try:
                udp_sock.close()
            except Exception:
                pass


def udp_loop(sock: socket.socket, udp_client: dict) -> None:
    upstream_map = {}
    while True:
        data, addr = sock.recvfrom(65535)
        log(f"socks5.udp.recv from={addr[0]}:{addr[1]} size={len(data)}")
        if udp_client["addr"] is None:
            udp_client["addr"] = addr
            log(f"socks5.udp.client.addr set={addr[0]}:{addr[1]}")
        if addr == udp_client["addr"]:
            if len(data) < 10:
                log("socks5.udp.drop too-short")
                continue
            frag = data[2]
            if frag != 0:
                log("socks5.udp.drop fragmented-not-supported")
                continue
            atyp = data[3]
            offset = 4
            if atyp == 1:
                host = socket.inet_ntoa(data[offset:offset+4])
                offset += 4
            elif atyp == 3:
                ln = data[offset]
                offset += 1
                host = data[offset:offset+ln].decode()
                offset += ln
            else:
                log(f"socks5.udp.drop atyp={atyp}")
                continue
            port = struct.unpack("!H", data[offset:offset+2])[0]
            offset += 2
            payload = data[offset:]
            upstream_map[(host, port)] = True
            log(f"socks5.udp.forward target={host}:{port} payload={len(payload)}")
            sock.sendto(data[offset:], (host, port))
        else:
            if not upstream_map:
                log(f"socks5.udp.drop unknown-upstream from={addr[0]}:{addr[1]}")
                continue
            packet = b"\x00\x00\x00" + pack_addr(addr[0], addr[1]) + data
            if udp_client["addr"] is not None:
                log(f"socks5.udp.return to={udp_client['addr'][0]}:{udp_client['addr'][1]} payload={len(data)}")
                sock.sendto(packet, udp_client["addr"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--username", default="")
    parser.add_argument("--password", default="")
    parser.add_argument("--advertise-host", default="")
    args = parser.parse_args()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", args.port))
    server.listen(32)
    log(f"socks5.listen {args.port} advertise={args.advertise_host or 'auto'}")
    while True:
        conn, _ = server.accept()
        threading.Thread(target=handle_tcp, args=(conn, args.username, args.password, args.advertise_host), daemon=True).start()


if __name__ == "__main__":
    raise SystemExit(main())
