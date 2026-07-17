#!/usr/bin/env python3
"""生成 NB 私有 CA、三节点双向 TLS 证书和 SOCKS PBKDF2 凭据。"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import secrets
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "build" / "security"


def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def chmod_private(path: pathlib.Path) -> None:
    try:
        path.chmod(0o600)
    except OSError:
        pass


def generate(out: pathlib.Path, username: str, password: str) -> None:
    openssl = shutil.which("openssl")
    if not openssl:
        raise SystemExit("未找到 openssl，请安装后重试")
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"安全目录非空，拒绝覆盖: {out}")
    out.mkdir(parents=True, mode=0o700, exist_ok=True)
    ca_key, ca_cert = out / "ca.key", out / "ca.pem"
    run(openssl, "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(ca_key))
    run(openssl, "req", "-x509", "-new", "-sha256", "-key", str(ca_key), "-days", "3650",
        "-subj", "/CN=NB Private CA", "-out", str(ca_cert))
    ext = out / "node.ext"
    ext.write_text("subjectAltName=DNS:nb.internal\nextendedKeyUsage=serverAuth,clientAuth\nkeyUsage=digitalSignature\n", encoding="ascii")
    for role in ("entry", "middle", "exit"):
        key, csr, cert = out / f"{role}.key", out / f"{role}.csr", out / f"{role}.pem"
        run(openssl, "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(key))
        run(openssl, "req", "-new", "-key", str(key), "-subj", f"/CN=nb-{role}", "-out", str(csr))
        run(openssl, "x509", "-req", "-sha256", "-in", str(csr), "-CA", str(ca_cert), "-CAkey", str(ca_key),
            "-CAcreateserial", "-days", "825", "-extfile", str(ext), "-out", str(cert))
        csr.unlink(); chmod_private(key)
    ext.unlink()
    salt = secrets.token_bytes(16)
    rounds = 300_000
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds, 32)
    users = out / "socks.users"
    users.write_text(f"{username}:{rounds}:{salt.hex()}:{digest.hex()}\n", encoding="ascii")
    chmod_private(ca_key); chmod_private(users)
    print(f"安全材料已生成: {out}")
    print("ca.key 仅保留在部署机，不会分发到节点")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    parser.add_argument("--username", default=os.environ.get("NB_SOCKS_USERNAME"))
    parser.add_argument("--password", default=os.environ.get("NB_SOCKS_PASSWORD"))
    args = parser.parse_args()
    if not args.username or not args.password:
        raise SystemExit("请通过 NB_SOCKS_USERNAME/NB_SOCKS_PASSWORD 或命令参数提供 SOCKS 凭据")
    if len(args.username.encode()) > 63 or len(args.password.encode()) > 255:
        raise SystemExit("SOCKS 用户名或密码过长")
    generate(args.out.resolve(), args.username, args.password)


if __name__ == "__main__":
    main()
