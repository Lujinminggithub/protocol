#!/usr/bin/env python3
"""拉取线上真实 hy2-front.json + 证书 SAN/SNI + obfs，用于推导手机 HY2 客户端配置。只读。"""
from __future__ import annotations
import deploy_fix as df


def main():
    ingress = df.load_hosts()["ingress"]
    cmd = (
        "set +e; "
        "echo '=== hy2-front.json (live) ==='; cat /etc/xgw/hy2-front.json 2>/dev/null; "
        "echo; echo '=== front cmdline ==='; "
        "for p in $(pgrep -f xgw-edge-server); do tr '\\0' ' ' < /proc/$p/cmdline; echo; done; "
        "echo '=== listen port (20023) ==='; ss -uanp 2>/dev/null | grep ':20023'; "
        "echo; echo '=== cert subject / SAN (what SNI client must use) ==='; "
        "openssl x509 -in /etc/xgw/gz-self.crt -noout -subject -ext subjectAltName 2>/dev/null; "
        "echo; echo '=== cert dates ==='; openssl x509 -in /etc/xgw/gz-self.crt -noout -dates 2>/dev/null; "
        "echo; echo '=== public ip seen from machine ==='; "
        "ip -4 addr show 2>/dev/null | grep -oE 'inet [0-9.]+' | head; "
    )
    code, out, err = df.run_remote(ingress, cmd, check=False, timeout=90)
    print(out)
    if err.strip():
        print("STDERR:", err[:1500])


if __name__ == "__main__":
    main()
