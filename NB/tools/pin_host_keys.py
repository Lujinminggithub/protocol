#!/usr/bin/env python3
"""首次引导后固定当前 NB 节点的 SSH 主机公钥。"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
import pathlib

import deploy


def main() -> None:
    parser=argparse.ArgumentParser()
    parser.add_argument("--out",type=pathlib.Path,default=deploy.SECURITY_DIR/"known_hosts")
    parser.add_argument("--force",action="store_true")
    args=parser.parse_args()
    if args.out.exists() and not args.force:raise SystemExit(f"文件已存在，拒绝覆盖: {args.out}")
    if os.environ.get("NB_SSH_INSECURE")!="1":raise SystemExit("首次固定必须显式设置 NB_SSH_INSECURE=1")
    lines=[]
    for role in reversed(deploy.deployment_roles()):
        host=deploy.LAB[role];client=deploy.connect(role);key=client.get_transport().get_remote_server_key();client.close()
        marker=host["host"] if int(host["port"])==22 else f"[{host['host']}]:{host['port']}"
        lines.append(f"{marker} {key.get_name()} {key.get_base64()}")
        digest=base64.b64encode(hashlib.sha256(key.asbytes()).digest()).decode().rstrip("=")
        print(f"{role} {marker} SHA256:{digest}")
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text("\n".join(lines)+"\n",encoding="ascii")
    try:args.out.chmod(0o600)
    except OSError:pass
    print(f"known_hosts 已写入: {args.out}")


if __name__=="__main__":main()
