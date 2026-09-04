#!/bin/bash
# 单机安全三角色回归，端口按进程号隔离。
set -euo pipefail
export NB_WORKER_LANE_PORTS=${NB_WORKER_LANE_PORTS:-on}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NB=${NB_BIN:-$ROOT/build/nb_node}
TMP=${TMPDIR:-/tmp}/nb-runtri-$$
MW=${NB_MIDDLE_WORKERS:-1}
EW=${NB_EXIT_WORKERS:-1}
IW=${NB_ENTRY_WORKERS:-1}
BASE=$(python3 - "$$" <<'PY'
import socket
import sys

start = int(sys.argv[1]) % 400
for attempt in range(400):
    base = 20000 + ((start + attempt) % 400) * 100
    sockets = []
    try:
        # Reserve the complete block while checking so worker-lane ports cannot
        # collide with a stale test process from an earlier interrupted run.
        for port in range(base, base + 100):
            for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                sock = socket.socket(socket.AF_INET, kind)
                sock.bind(("127.0.0.1", port))
                sockets.append(sock)
    except OSError:
        for sock in sockets:
            sock.close()
        continue
    for sock in sockets:
        sock.close()
    print(base)
    break
else:
    raise SystemExit("no free 100-port runtri block")
PY
)
HTTP_PORT=$BASE; ECHO_PORT=$((BASE+1)); EXIT_PORT=$((BASE+10)); MIDDLE_PORT=$((BASE+45)); ENTRY_PORT=$((BASE+80))
export HTTP_PORT EXIT_PORT MIDDLE_PORT ENTRY_PORT ECHO_PORT
mkdir -p "$TMP/www" "$TMP/pki"
BACKGROUND_PIDS=()
trap 'rc=$?; trap - EXIT; if [ ${#BACKGROUND_PIDS[@]} -gt 0 ]; then kill "${BACKGROUND_PIDS[@]}" 2>/dev/null || true; fi; pkill -P $$ 2>/dev/null || true; rm -f /run/nb-middle-{0..31}.ctl /run/nb-exit-{0..31}.ctl; if [ $rc -eq 0 ] || [ "${NB_KEEP_TMP:-0}" != 1 ]; then rm -rf "$TMP"; else echo "FAILED LOGS: $TMP" >&2; fi; exit "$rc"' EXIT

if [ ! -x "$NB" ]; then
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$ROOT/build" -j"$(nproc)"
fi
mkdir -p "$TMP/cfg"
cp "$NB" "$TMP/nb_node"
NB="$TMP/nb_node"
cat >"$TMP/cfg/log4c.json" <<EOF
{
  "log_dir": "$TMP"
}
EOF

cat >"$TMP/pki/node.ext" <<'EOF'
subjectAltName=DNS:nb.internal
extendedKeyUsage=serverAuth,clientAuth
keyUsage=digitalSignature
EOF
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/pki/ca.key" >/dev/null 2>&1
openssl req -x509 -new -sha256 -key "$TMP/pki/ca.key" -days 1 -subj /CN=NB-Test-CA -out "$TMP/pki/ca.pem" >/dev/null 2>&1
for role in entry middle exit; do
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$TMP/pki/$role.key" >/dev/null 2>&1
  openssl req -new -key "$TMP/pki/$role.key" -subj "/CN=nb-$role" -out "$TMP/pki/$role.csr" >/dev/null 2>&1
  openssl x509 -req -sha256 -in "$TMP/pki/$role.csr" -CA "$TMP/pki/ca.pem" -CAkey "$TMP/pki/ca.key" \
    -CAcreateserial -days 1 -extfile "$TMP/pki/node.ext" -out "$TMP/pki/$role.pem" >/dev/null 2>&1
  chmod 600 "$TMP/pki/$role.key"
done

cat >"$TMP/whitelist.conf" <<EOF
ip 127.0.0.0/8
port $HTTP_PORT
port $ECHO_PORT
EOF
cat >"$TMP/exit_routes.conf" <<EOF
route local H:127.0.0.1:$EXIT_PORT 1
EOF
cat >"$TMP/tenant.conf" <<'EOF'
tenant nbtest 64 16 100000 0
EOF
python3 - "$TMP/socks.users" <<'PY'
import hashlib,secrets,sys
salt=secrets.token_bytes(16);rounds=100000
digest=hashlib.pbkdf2_hmac("sha256",b"nb-test-password",salt,rounds,32)
open(sys.argv[1],"w",encoding="ascii").write(f"nbtest:{rounds}:{salt.hex()}:{digest.hex()}\n")
PY
chmod 600 "$TMP/socks.users"
echo HELLO_NB_TUNNEL_OK >"$TMP/www/test.txt"
head -c 300000 /dev/urandom | base64 >"$TMP/www/big.txt"

(cd "$TMP/www" && exec python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 >"$TMP/http.log" 2>&1) &
HTTP_PID=$!
BACKGROUND_PIDS+=("$HTTP_PID")
python3 - "$HTTP_PORT" >"$TMP/udp-target.log" 2>&1 <<'PY' &
import socket,sys,time
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(("127.0.0.1",int(sys.argv[1])))
data,peer=s.recvfrom(65535);s.sendto(data,peer)
for _ in range(3):time.sleep(2);s.sendto(b"late-"+data,peer)
s.close()
PY
UDP_TARGET_PID=$!
BACKGROUND_PIDS+=("$UDP_TARGET_PID")
python3 - "$ECHO_PORT" >"$TMP/echo-target.log" 2>&1 <<'PY' &
import socket,sys,threading
listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
listener.bind(("127.0.0.1",int(sys.argv[1])));listener.listen(32)
def serve(conn):
    with conn:
        while True:
            data=conn.recv(65536)
            if not data:break
            conn.sendall(data)
while True:
    conn,_=listener.accept();threading.Thread(target=serve,args=(conn,),daemon=True).start()
PY
ECHO_TARGET_PID=$!
BACKGROUND_PIDS+=("$ECHO_TARGET_PID")
for _ in $(seq 1 50); do
  if curl -fsS --noproxy "*" "http://127.0.0.1:$HTTP_PORT/test.txt" --max-time 1 | grep -qx HELLO_NB_TUNNEL_OK; then
    HTTP_READY=1
    break
  fi
  kill -0 "$HTTP_PID" "$UDP_TARGET_PID" "$ECHO_TARGET_PID" 2>/dev/null || break
  sleep .1
done
if [ "${HTTP_READY:-0}" != 1 ]; then
  echo "runtri local targets failed to start" >&2
  cat "$TMP/http.log" >&2 || true
  cat "$TMP/udp-target.log" >&2 || true
  cat "$TMP/echo-target.log" >&2 || true
  exit 1
fi
if [ "$EW" -gt 1 ]; then
  NB_CONTROL_DIR="$TMP" python3 "$ROOT/tools/nb_supervisor.py" --workers "$EW" -- "$NB" -r exit -p "$EXIT_PORT" -c "$TMP/pki/exit.pem" -k "$TMP/pki/exit.key" -a "$TMP/pki/ca.pem" -W "$TMP/whitelist.conf" >"$TMP/exit.log" 2>&1 &
  EXIT_SUP=$!
else
  "$NB" -r exit -p "$EXIT_PORT" -c "$TMP/pki/exit.pem" -k "$TMP/pki/exit.key" -a "$TMP/pki/ca.pem" -W "$TMP/whitelist.conf" -C "$TMP/exit.ctl" >"$TMP/exit.log" 2>&1 &
fi
if [ "$MW" -gt 1 ]; then
  NB_CONTROL_DIR="$TMP" python3 "$ROOT/tools/nb_supervisor.py" --workers "$MW" -- "$NB" -r middle -p "$MIDDLE_PORT" -c "$TMP/pki/middle.pem" -k "$TMP/pki/middle.key" -a "$TMP/pki/ca.pem" >"$TMP/middle.log" 2>&1 &
  MIDDLE_SUP=$!
else
  "$NB" -r middle -p "$MIDDLE_PORT" -c "$TMP/pki/middle.pem" -k "$TMP/pki/middle.key" -a "$TMP/pki/ca.pem" -C "$TMP/middle.ctl" >"$TMP/middle.log" 2>&1 &
fi
sleep 1
ENTRY_CMD=("$NB" -r entry -l "$ENTRY_PORT" -n 127.0.0.1 -N "$MIDDLE_PORT" -S -U "$TMP/socks.users" -Q "$TMP/tenant.conf" -W "$TMP/whitelist.conf" \
  -E "$TMP/exit_routes.conf" -c "$TMP/pki/entry.pem" -k "$TMP/pki/entry.key" -a "$TMP/pki/ca.pem")
if [ "$IW" -gt 1 ]; then
  NB_UDP_CONTROL_GRACE_MS=500 NB_CONTROL_DIR="$TMP" python3 "$ROOT/tools/nb_supervisor.py" --workers "$IW" -- "${ENTRY_CMD[@]}" >"$TMP/entry.log" 2>&1 &
  ENTRY_SUP=$!
else
  NB_UDP_CONTROL_GRACE_MS=500 "${ENTRY_CMD[@]}" -C "$TMP/entry.ctl" >"$TMP/entry.log" 2>&1 &
fi
sleep 2

CONTROLS=()
if [ "$IW" -eq 1 ]; then CONTROLS+=("$TMP/entry.ctl"); else for i in $(seq 0 $((IW-1))); do CONTROLS+=("$TMP/nb-entry-$i.ctl"); done; fi
ENTRY_CONTROL=${CONTROLS[0]}
if [ "$MW" -eq 1 ]; then CONTROLS+=("$TMP/middle.ctl"); else for i in $(seq 0 $((MW-1))); do CONTROLS+=("$TMP/nb-middle-$i.ctl"); done; fi
if [ "$EW" -eq 1 ]; then CONTROLS+=("$TMP/exit.ctl"); else for i in $(seq 0 $((EW-1))); do CONTROLS+=("$TMP/nb-exit-$i.ctl"); done; fi
for path in "${CONTROLS[@]}"; do
  ready=0
  for _ in $(seq 1 100); do [ -S "$path" ] && { ready=1;break; };sleep .1;done
  test "$ready" -eq 1
done
python3 - "${CONTROLS[@]}" <<'PY'
import json,socket,sys
def query(path,command):
    client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.connect(path);client.sendall(command.encode()+b"\n")
    chunks=[]
    while True:
        part=client.recv(4096)
        if not part:break
        chunks.append(part)
    client.close();return json.loads(b"".join(chunks))
for path in sys.argv[1:]:
    reply=query(path,"health")
    assert reply["status"]=="ok",(path,reply)
tenants=query(sys.argv[1],"tenants");routes=query(sys.argv[1],"routes")
assert tenants["tenants"][0]["name"]=="nbtest",tenants
assert routes["routes"][0]["name"]=="local",routes
PY

if curl -fsS --noproxy "" --proxy-user nbtest:wrong-password --socks5-hostname 127.0.0.1:$ENTRY_PORT \
  http://127.0.0.1:$HTTP_PORT/test.txt --max-time 3 >/dev/null 2>&1; then
  echo "错误：SOCKS 错误密码未被拒绝" >&2;exit 1
fi

for _ in 1 2 3; do
  if curl -fsS --noproxy "" --proxy-user nbtest:nb-test-password --socks5-hostname 127.0.0.1:$ENTRY_PORT \
    http://127.0.0.2:$HTTP_PORT/test.txt --max-time 2 >/dev/null 2>&1; then
    echo "ERROR: unavailable target unexpectedly succeeded" >&2;exit 1
  fi
done
python3 - "$ENTRY_CONTROL" <<'PY'
import json,socket,sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.connect(sys.argv[1]);s.sendall(b"routes\n")
data=b""
while True:
    part=s.recv(4096)
    if not part:break
    data+=part
s.close();route=json.loads(data)["routes"][0]
assert route["healthy"] is True and route["rejected"]==0,route
print("RESULT PASS: short business sessions keep route health neutral")
PY

python3 - <<'PY'
import concurrent.futures
import os
import socket
import threading
import time

entry=("127.0.0.1",int(os.environ["ENTRY_PORT"]))
start=threading.Event()
def greeting(_):
    sock=socket.create_connection(entry,timeout=2)
    sock.settimeout(2)
    start.wait()
    began=time.monotonic()
    sock.sendall(b"\x05\x01\x02")
    reply=sock.recv(2)
    elapsed=time.monotonic()-began
    sock.close()
    if reply!=b"\x05\x02":raise RuntimeError(f"bad SOCKS greeting reply: {reply!r}")
    return elapsed

with concurrent.futures.ThreadPoolExecutor(max_workers=64) as executor:
    futures=[executor.submit(greeting,index) for index in range(64)]
    time.sleep(0.1)
    start.set()
    elapsed=[future.result(timeout=3) for future in futures]
assert max(elapsed)<2.0,max(elapsed)
print(f"RESULT PASS: 64 concurrent SOCKS greetings max={max(elapsed):.3f}s")
PY

python3 - <<'PY'
import math,socket,struct

def recv_exact(sock,size):
    data=bytearray()
    while len(data)<size:
        chunk=sock.recv(size-len(data))
        if not chunk:raise RuntimeError(f"SOCKS response ended at {len(data)}/{size}")
        data.extend(chunk)
    return bytes(data)

import os
sock=socket.create_connection(("127.0.0.1",int(os.environ["ENTRY_PORT"])),timeout=10);sock.settimeout(20)
sock.sendall(b"\x05\x01\x02");assert recv_exact(sock,2)==b"\x05\x02"
user=b"nbtest";password=b"nb-test-password"
sock.sendall(b"\x01"+bytes([len(user)])+user+bytes([len(password)])+password)
assert recv_exact(sock,2)==b"\x01\x00"
target=b"nb-probe-echo.internal"
sock.sendall(b"\x05\x01\x00\x03"+bytes([len(target)])+target+struct.pack("!H",9))
reply=recv_exact(sock,4);assert reply[:2]==b"\x05\x00",reply
if reply[3]==1:recv_exact(sock,6)
elif reply[3]==3:recv_exact(sock,recv_exact(sock,1)[0]+2)
elif reply[3]==4:recv_exact(sock,18)
else:raise RuntimeError(f"unknown SOCKS ATYP {reply[3]}")
pattern=bytes((i*31+17)&0xff for i in range(4096));size=256*1024
expected=(pattern*math.ceil(size/len(pattern)))[:size]
sock.sendall(expected);sock.shutdown(socket.SHUT_WR);received=bytearray()
while True:
    chunk=sock.recv(65536)
    if not chunk:break
    received.extend(chunk)
sock.close()
assert bytes(received)==expected,f"half-close echo truncated: {len(received)}/{len(expected)}"
print("RESULT PASS: 256KB half-close echo integrity")
PY

python3 - <<'PY'
import os,socket,struct,time
def exact(s,n):
    out=b""
    while len(out)<n:
        part=s.recv(n-len(out))
        if not part:raise RuntimeError("short SOCKS response")
        out+=part
    return out
def address(s):
    head=exact(s,4);atyp=head[3]
    if atyp==1:host=socket.inet_ntoa(exact(s,4))
    elif atyp==3:host=exact(s,exact(s,1)[0]).decode()
    elif atyp==4:host=socket.inet_ntop(socket.AF_INET6,exact(s,16))
    else:raise RuntimeError(f"bad ATYP {atyp}")
    return head,host,struct.unpack("!H",exact(s,2))[0]
entry=("127.0.0.1",int(os.environ["ENTRY_PORT"]));target_port=int(os.environ["HTTP_PORT"])
tcp=socket.create_connection(entry,timeout=5);tcp.sendall(b"\x05\x01\x02");assert exact(tcp,2)==b"\x05\x02"
u=b"nbtest";p=b"nb-test-password";tcp.sendall(b"\x01"+bytes([len(u)])+u+bytes([len(p)])+p);assert exact(tcp,2)==b"\x01\x00"
tcp.sendall(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00");head,host,port=address(tcp);assert head[1]==0
if host=="0.0.0.0":host="127.0.0.1"
udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.settimeout(5);payload=b"NB_UDP_LIFECYCLE"
packet=b"\x00\x00\x00\x01"+socket.inet_aton("127.0.0.1")+struct.pack("!H",target_port)+payload
udp.sendto(packet,(host,port));reply,_=udp.recvfrom(65535);assert reply.endswith(payload),reply
tcp.close();udp.close();time.sleep(5)
print("RESULT PASS: SOCKS UDP lifecycle trigger")
PY
grep -q 'middle udp flow close.*reason=udp-peer-close' "$TMP/nb-middle.log"
grep -q 'exit udp flow close.*reason=udp-peer-close' "$TMP/nb-exit.log"
if grep -q 'udp datagram reject stage=handler rc=-10' "$TMP/nb-entry.log"; then
  echo "ERROR: stale S2C UDP datagram reached released entry child" >&2;exit 1
fi
echo "RESULT PASS: UDP child close synchronized across entry/middle/exit"

UDP_ECHO_CONTROLS=()
for control in "${CONTROLS[@]}"; do UDP_ECHO_CONTROLS+=(--control "$control"); done
python3 "$ROOT/tools/runtri_udp_probe_echo.py" --entry-port "$ENTRY_PORT" --username nbtest \
  --password-stdin "${UDP_ECHO_CONTROLS[@]}" <<<"nb-test-password"

for i in $(seq 1 10); do
  curl -fsS -o /dev/null -w "try$i first_byte=%{time_starttransfer}s http=%{http_code}\n" \
    --noproxy "" --proxy-user nbtest:nb-test-password --socks5-hostname 127.0.0.1:$ENTRY_PORT \
    http://127.0.0.1:$HTTP_PORT/test.txt --max-time 10
done
seq 1 16 | xargs -P8 -I{} curl -fsS -o /dev/null --noproxy "" --proxy-user nbtest:nb-test-password \
  --socks5-hostname 127.0.0.1:$ENTRY_PORT http://127.0.0.1:$HTTP_PORT/test.txt --max-time 10
echo "RESULT PASS: 16 路并发"
if [ "$MW" -gt 1 ]; then
  sleep 2
  export MIDDLE_SUP
  python3 - "$TMP/lstream.started" <<'PY' &
import os,socket,struct,sys,threading,time
def exact(sock,size):
    out=bytearray()
    while len(out)<size:
        part=sock.recv(size-len(out))
        if not part:raise RuntimeError(f"short response {len(out)}/{size}")
        out.extend(part)
    return bytes(out)
s=socket.create_connection(("127.0.0.1",int(os.environ["ENTRY_PORT"])),timeout=10);s.settimeout(60)
s.sendall(b"\x05\x01\x02");assert exact(s,2)==b"\x05\x02"
u=b"nbtest";p=b"nb-test-password";s.sendall(b"\x01"+bytes([len(u)])+u+bytes([len(p)])+p);assert exact(s,2)==b"\x01\x00"
s.sendall(b"\x05\x01\x00\x01"+socket.inet_aton("127.0.0.1")+struct.pack("!H",int(os.environ["ECHO_PORT"])))
head=exact(s,4);assert head[:2]==b"\x05\x00",head
exact(s,6 if head[3]==1 else 18)
payload=bytes((i*73+19)&255 for i in range(2*1024*1024));received=bytearray();failure=[]
def reader():
    try:
        while len(received)<len(payload):received.extend(s.recv(min(65536,len(payload)-len(received))))
    except BaseException as exc:failure.append(exc)
reader_thread=threading.Thread(target=reader,daemon=True);reader_thread.start();open(sys.argv[1],"w").close()
for offset in range(0,len(payload),4096):
    s.sendall(payload[offset:offset+4096]);time.sleep(.001)
s.shutdown(socket.SHUT_WR)
reader_thread.join(60)
if failure:raise failure[0]
assert not reader_thread.is_alive(),f"logical resume receive timeout {len(received)}/{len(payload)}"
assert bytes(received)==payload,f"logical resume payload mismatch {len(received)}/{len(payload)}"
s.close()
print("RESULT PASS: same logical TCP stream survived middle connection loss")
PY
  LSTREAM_CLIENT=$!
  for _ in $(seq 1 100); do [ -f "$TMP/lstream.started" ] && break;sleep .05;done
  test -f "$TMP/lstream.started";sleep .1
  victims=$(pgrep -P "$MIDDLE_SUP");test -n "$victims";kill -9 $victims
  wait "$LSTREAM_CLIENT"
  grep -q 'logical flow resume confirmed' "$TMP/nb-entry.log"
  for _ in $(seq 1 100); do [ "$(pgrep -P "$MIDDLE_SUP" | wc -l)" -eq "$MW" ] && break; sleep .1; done
  test "$(pgrep -P "$MIDDLE_SUP" | wc -l)" -eq "$MW"
  echo "RESULT PASS: middle worker 故障后主动 payload 恢复"
fi
expected_hash=$(sha256sum "$TMP/www/big.txt" | cut -d' ' -f1);payload_ok=0
for _ in $(seq 1 30); do
  if curl -fsS --noproxy "" --proxy-user nbtest:nb-test-password --socks5-hostname 127.0.0.1:$ENTRY_PORT \
    http://127.0.0.1:$HTTP_PORT/big.txt -o "$TMP/got" --max-time 20 &&
    [ "$expected_hash" = "$(sha256sum "$TMP/got" | cut -d' ' -f1)" ]; then payload_ok=1;break;fi
  sleep 1
done
test "$payload_ok" -eq 1
echo "RESULT PASS：三跳 payload 完整"
