#!/bin/bash
# 单机 loopback 三跳回归 —— 一台有 picoquic 的 Linux 机上验证 entry+middle+exit 协议逻辑,
# 不依赖真机三跳。链路: TCP:8080(entry) -> QUIC:9444(middle) -> QUIC:9443(exit) -> HTTP:9000
#
# 用法: bash scripts/runtri.sh <picoquic_dir>   (默认 /root/poc/picoquic)
set -e
PQ=${1:-/root/poc/picoquic}
CERT=$PQ/certs/cert.pem
KEY=$PQ/certs/key.pem
NB=./nb_node

# 编译(若二进制不存在)
if [ ! -x "$NB" ]; then
  echo "[build] nb_node ..."
  gcc -O2 -Wall -std=c11 -D_GNU_SOURCE -pthread -o nb_node src/nb_node.c src/log/log4c.c \
    -Isrc -I"$PQ/picoquic" -I"$PQ/loglib" -I"$PQ/picotls/include" \
    -Wl,--start-group "$PQ/libpicoquic-core.a" "$PQ/libpicoquic-log.a" \
    "$PQ/picotls/libpicotls-openssl.a" "$PQ/picotls/libpicotls-core.a" \
    "$PQ/picotls/libpicotls-minicrypto.a" -Wl,--end-group -lssl -lcrypto -lpthread
fi

# 目标 http 服务 + 测试文件
mkdir -p /tmp/nbwww
echo HELLO_NB_TUNNEL_OK >/tmp/nbwww/test.txt
head -c 300000 /dev/urandom | base64 >/tmp/nbwww/big.txt
pkill -9 -x nb_node 2>/dev/null || true
pkill -9 -f 'python3 -m http.server 9000' 2>/dev/null || true
sleep 0.5
( cd /tmp/nbwww && setsid nohup python3 -m http.server 9000 </dev/null >/tmp/nb_http.log 2>&1 & )

# 起 exit -> middle -> entry
( setsid nohup "$NB" -r exit   -p 9443 -c "$CERT" -k "$KEY" </dev/null >/tmp/nb_exit.log 2>&1 & )
( setsid nohup "$NB" -r middle -p 9444 -c "$CERT" -k "$KEY" </dev/null >/tmp/nb_mid.log  2>&1 & )
sleep 1
( setsid nohup "$NB" -r entry -l 8080 -n 127.0.0.1 -N 9444 \
    -R "H:127.0.0.1:9443,T:127.0.0.1:9000" </dev/null >/tmp/nb_entry.log 2>&1 & )
sleep 2

echo "=== 多 stream 回归(10 次) ==="
for i in $(seq 1 10); do
  curl -s -o /dev/null -w "try$i first_byte=%{time_starttransfer}s http=%{http_code}\n" \
    http://127.0.0.1:8080/test.txt --max-time 10
done
echo "=== 300KB md5 一致性 ==="
curl -s http://127.0.0.1:8080/big.txt -o /tmp/nb_got --max-time 15
echo "src_md5=$(md5sum /tmp/nbwww/big.txt | awk '{print $1}')"
echo "got_md5=$(md5sum /tmp/nb_got      | awk '{print $1}')"

pkill -9 -x nb_node 2>/dev/null || true
pkill -9 -f 'python3 -m http.server 9000' 2>/dev/null || true
echo "done (日志: /tmp/nb_entry.log nb_mid.log nb_exit.log; log4c: ./logs/nb-*.log)"
