#!/usr/bin/env python3
"""验证 NB 自包含多平台构建 —— 在 kz 上用 vendored third_party 构建两条路径:
  A) prebuilt 直链:   CMake 检测到 prebuilt/<platform>/*.a, 直接链接
  B) 从源码现编:      删掉 prebuilt, CMake 触发 build_libs.sh 从 vendored src 两阶段构建

两条都产出 nb_node 即证明: 源码 vendor 完整 + 构建自包含 + 多平台就绪(换平台=走 B)。
利用 vendor 时残留在 kz /tmp 的 pqsrc.tgz/pqa.tgz 重建 third_party, 避免慢链路重传。
"""
from __future__ import annotations
import io, json, pathlib
import paramiko

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAB = json.loads((ROOT / "tools" / "lab-hosts.json").read_text(encoding="utf-8"))
KZ = LAB["exit"]
NBV = "/root/nb-verify"


def main():
    c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(KZ["host"], port=KZ["port"], username=KZ["user"], password=KZ["password"],
              timeout=25, banner_timeout=25, auth_timeout=25, allow_agent=False, look_for_keys=False)

    def run(cmd, t=400):
        _i, o, e = c.exec_command(cmd, timeout=t)
        return o.read().decode("utf-8", "replace") + e.read().decode("utf-8", "replace")

    def put(local, remote, mode=0o644):
        run(f"mkdir -p $(dirname {remote})")
        sf = c.open_sftp(); sf.putfo(io.BytesIO(pathlib.Path(local).read_bytes()), remote)
        sf.chmod(remote, mode); sf.close()

    # 1) 从 /tmp tgz 重建 vendored third_party
    print("### 重建 third_party(从 kz /tmp tgz) ...")
    print(run(f"ls -l /tmp/pqsrc.tgz /tmp/pqa.tgz 2>&1 || echo 'tgz 丢失, 请先重跑 vendor_picoquic.py'"))
    run(f"rm -rf {NBV} && mkdir -p {NBV}/src/log "
        f"{NBV}/third_party/picoquic/src {NBV}/third_party/picoquic/prebuilt/linux-x86_64")
    run(f"tar xf /tmp/pqsrc.tgz -C {NBV}/third_party/picoquic/src")
    run(f"tar xf /tmp/pqa.tgz -C {NBV}/third_party/picoquic/prebuilt/linux-x86_64")

    # 2) 上传 NB 自有源码 + 构建脚本
    put(ROOT / "src" / "nb_node.c", f"{NBV}/src/nb_node.c")
    put(ROOT / "src" / "log" / "log4c.c", f"{NBV}/src/log/log4c.c")
    put(ROOT / "src" / "log" / "log4c.h", f"{NBV}/src/log/log4c.h")
    put(ROOT / "CMakeLists.txt", f"{NBV}/CMakeLists.txt")
    put(ROOT / "third_party" / "picoquic" / "build_libs.sh",
        f"{NBV}/third_party/picoquic/build_libs.sh", mode=0o755)

    # 路径 A: prebuilt 直链
    print("\n### 路径A: prebuilt 直链 ###")
    print(run(f"cd {NBV} && rm -rf build-a && cmake -S . -B build-a >/tmp/ca.log 2>&1; "
              f"cmake --build build-a -j$(nproc) >>/tmp/ca.log 2>&1; echo cmake_rc=$?; "
              f"ls -l build-a/nb_node 2>&1 && echo A_BUILT_OK || echo A_FAIL; "
              f"grep -i 'using prebuilt\\|error' /tmp/ca.log | head -5"))

    # 路径 B: 删 prebuilt -> 从源码现编
    print("\n### 路径B: 从 vendored 源码现编 ###")
    print(run(f"cd {NBV} && rm -rf third_party/picoquic/prebuilt build-b && "
              f"cmake -S . -B build-b >/tmp/cb.log 2>&1; "
              f"cmake --build build-b -j$(nproc) >>/tmp/cb.log 2>&1; echo cmake_rc=$?; "
              f"ls -l build-b/nb_node 2>&1 && echo B_BUILT_OK || echo B_FAIL; "
              f"grep -i 'building from source\\|error' /tmp/cb.log | head -8", t=500))
    print("\n### 路径B 详细日志尾部(若失败) ###")
    print(run("tail -20 /tmp/cb.log"))
    c.close()


if __name__ == "__main__":
    main()
