#!/bin/bash
# 单机安全三角色回归：entry:8080 -> middle:9444 -> exit:9443 -> HTTP:9000
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NB=${NB_BIN:-$ROOT/build/nb_node}
TMP=${TMPDIR:-/tmp}/nb-runtri-$$
mkdir -p "$TMP/www" "$TMP/pki"
trap 'pkill -P $$ 2>/dev/null || true; rm -rf "$TMP"' EXIT

if [ ! -x "$NB" ]; then
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$ROOT/build" -j"$(nproc)"
fi

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

cat >"$TMP/whitelist.conf" <<'EOF'
ip 127.0.0.0/8
port 9000
EOF
cat >"$TMP/exit_routes.conf" <<'EOF'
route local H:127.0.0.1:9443 1
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

(cd "$TMP/www" && python3 -m http.server 9000 >"$TMP/http.log" 2>&1) &
"$NB" -r exit -p 9443 -c "$TMP/pki/exit.pem" -k "$TMP/pki/exit.key" -a "$TMP/pki/ca.pem" \
  -W "$TMP/whitelist.conf" -C "$TMP/exit.ctl" >"$TMP/exit.log" 2>&1 &
"$NB" -r middle -p 9444 -c "$TMP/pki/middle.pem" -k "$TMP/pki/middle.key" -a "$TMP/pki/ca.pem" \
  -C "$TMP/middle.ctl" >"$TMP/middle.log" 2>&1 &
sleep 1
"$NB" -r entry -l 8080 -n 127.0.0.1 -N 9444 -S -U "$TMP/socks.users" -W "$TMP/whitelist.conf" \
  -E "$TMP/exit_routes.conf" -c "$TMP/pki/entry.pem" -k "$TMP/pki/entry.key" -a "$TMP/pki/ca.pem" -C "$TMP/entry.ctl" \
  >"$TMP/entry.log" 2>&1 &
sleep 2

if curl -fsS --noproxy "" --proxy-user nbtest:wrong-password --socks5-hostname 127.0.0.1:8080 \
  http://127.0.0.1:9000/test.txt --max-time 3 >/dev/null 2>&1; then
  echo "错误：SOCKS 错误密码未被拒绝" >&2;exit 1
fi
python3 - "$TMP/entry.ctl" "$TMP/middle.ctl" "$TMP/exit.ctl" <<'PY'
import json,socket,sys
for path in sys.argv[1:]:
    client=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);client.connect(path);client.sendall(b"health\n")
    reply=json.loads(client.recv(1024));client.close()
    assert reply["status"]=="ok",(path,reply)
PY

for i in $(seq 1 10); do
  curl -fsS -o /dev/null -w "try$i first_byte=%{time_starttransfer}s http=%{http_code}\n" \
    --noproxy "" --proxy-user nbtest:nb-test-password --socks5-hostname 127.0.0.1:8080 \
    http://127.0.0.1:9000/test.txt --max-time 10
done
curl -fsS --noproxy "" --proxy-user nbtest:nb-test-password --socks5-hostname 127.0.0.1:8080 \
  http://127.0.0.1:9000/big.txt -o "$TMP/got" --max-time 20
test "$(sha256sum "$TMP/www/big.txt" | cut -d' ' -f1)" = "$(sha256sum "$TMP/got" | cut -d' ' -f1)"
echo "RESULT PASS：三跳 payload 完整"
