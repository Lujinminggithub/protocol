#!/usr/bin/env python3
"""只在 ingress 远端编译验证（cc -Wall -Wextra -pedantic），不分发、不重启。"""
from __future__ import annotations
import deploy_fix as df


def main():
    hosts = df.load_hosts()
    ingress = hosts["ingress"]
    # 上传最新源码到 ingress（仅 ingress，不碰 relay/egress）
    bundle = df.build_source_bundle()
    df.run_remote(ingress, f"mkdir -p {df.WORK_DIR}/configs {df.WORK_DIR}/logs")
    df.upload_bytes(ingress, bundle, f"{df.WORK_DIR}/xgw-src-verify.tar.gz")
    df.run_remote(ingress, f"cd {df.WORK_DIR} && rm -rf verify_src && mkdir verify_src && tar -xzf xgw-src-verify.tar.gz -C verify_src")
    print("[ship] sources uploaded to ingress verify_src", flush=True)
    # 编译到临时输出，捕获全部告警/错误
    compile_cmd = (
        f"cd {df.WORK_DIR}/verify_src && "
        'COMPILER="$(command -v cc || command -v gcc || command -v clang)"; '
        '[ -n "$COMPILER" ] || { echo "no compiler found" >&2; exit 127; }; '
        'echo "using $COMPILER"; '
        '"$COMPILER" -std=c11 -O2 -Wall -Wextra -pedantic -pthread -Iinclude -Isrc/log '
        "src/main.c src/config.c src/policy.c src/protocol.c src/crypto.c src/security.c src/control.c src/cc.c "
        "src/session.c src/frame.c src/transport_udp.c src/dataplane.c src/runtime.c src/route.c src/bridge.c src/acl.c "
        "src/pool.c src/tuning.c src/obfs.c src/outbound.c src/tun_stub.c src/tun_linux.c src/afxdp_stub.c "
        'src/afxdp_linux.c src/log/log4c.c -o xgw-verify 2>&1; echo "EXIT=$?"; ls -la xgw-verify 2>/dev/null'
    )
    code, out, err = df.run_remote(ingress, compile_cmd, check=False, timeout=300)
    print(out)
    if err.strip():
        print("STDERR:\n", err)


if __name__ == "__main__":
    main()
