#!/usr/bin/env python3
"""从构建机(kz)抽取 picoquic 完整源码 + 预编译静态库, 固化进 NB/third_party/picoquic/。

产出布局(全部入 git, 自包含多平台):
  third_party/picoquic/
    src/                     picoquic 完整源码树(picoquic + picotls + cifra/micro-ecc/picotest)
    prebuilt/linux-x86_64/   当前平台预编译静态库(.a) 作缓存

源码用于按目标平台从零构建(build_libs.sh 两阶段 cmake); prebuilt 命中则免现编。
一次性 vendor 工具; 升级 picoquic 版本时重跑。
"""
from __future__ import annotations
import io, json, pathlib, tarfile, shutil
import paramiko

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAB = json.loads((ROOT / "tools" / "lab-hosts.json").read_text(encoding="utf-8"))
PQ = LAB["paths"]["picoquic"]
DST = ROOT / "third_party" / "picoquic"
KZ = LAB["exit"]


def main():
    c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(KZ["host"], port=KZ["port"], username=KZ["user"], password=KZ["password"],
              timeout=25, banner_timeout=25, auth_timeout=25, allow_agent=False, look_for_keys=False)

    def run(cmd, t=120):
        _i, o, e = c.exec_command(cmd, timeout=t)
        return o.read().decode() + e.read().decode()

    def fetch(path, t=400):
        """exec cat 二进制读取(避开 sftp 逐块 ACK, 慢链路更快)。"""
        _i, o, _e = c.exec_command(f"cat {path}", timeout=t)
        return o.read()

    # 1) 打包源码(完整树,保证顶层 CMakeLists add_executable 引用的源都在;
    #    只排除 .git / 构建中间物 / ELF 可执行产物(根目录 picoquicdemo 等))
    src_pack = (
        f"cd {PQ} && rm -f /tmp/pqsrc.tgz && "
        "find . -type f -executable "
        "-not -name '*.sh' -not -name '*.py' -not -name '*.pl' -not -name '*.t' "
        "-not -name '*.cmake' -not -name '*.c' -not -name '*.h' > /tmp/pqexcl.txt && "
        "tar czf /tmp/pqsrc.tgz "
        "--exclude='.git' --exclude='*.o' --exclude='*.a' --exclude='*.qlog' "
        "--exclude=CMakeFiles --exclude='build-*' --exclude=CMakeCache.txt "
        "--exclude='cmake_install.cmake' --exclude=CTestTestfile.cmake --exclude=Testing "
        "--exclude-from=/tmp/pqexcl.txt . && "
        "echo SRC_OK size=$(du -h /tmp/pqsrc.tgz|cut -f1)"
    )
    print(run(src_pack))
    # 2) 打包预编译 .a
    a_pack = (
        f"cd {PQ} && rm -rf /tmp/pqa && mkdir -p /tmp/pqa && "
        "cp libpicoquic-core.a libpicoquic-log.a "
        "picotls/libpicotls-openssl.a picotls/libpicotls-core.a picotls/libpicotls-minicrypto.a /tmp/pqa/ && "
        "tar czf /tmp/pqa.tgz -C /tmp/pqa . && echo A_OK n=$(ls /tmp/pqa/*.a|wc -l)"
    )
    print(run(a_pack))

    print("下载源码 tgz ...")
    src_data = fetch("/tmp/pqsrc.tgz")
    print(f"  源码 {len(src_data)//1024}KB")
    print("下载预编译库 tgz ...")
    a_data = fetch("/tmp/pqa.tgz")
    print(f"  预编译 {len(a_data)//1024}KB")
    c.close()

    # 本地解压(只清 src/prebuilt, 保留 build_libs.sh 等自有脚本)
    for sub in ("src", "prebuilt"):
        if (DST / sub).exists():
            shutil.rmtree(DST / sub)
    src_dst = DST / "src"; src_dst.mkdir(parents=True, exist_ok=True)
    pre_dst = DST / "prebuilt" / "linux-x86_64"; pre_dst.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(src_data), mode="r:gz") as t:
        t.extractall(src_dst)
    with tarfile.open(fileobj=io.BytesIO(a_data), mode="r:gz") as t:
        t.extractall(pre_dst)

    srcs = list(src_dst.rglob("*.c")) + list(src_dst.rglob("*.h"))
    libs = list(pre_dst.glob("*.a"))
    total = sum(f.stat().st_size for f in src_dst.rglob("*") if f.is_file())
    print(f"vendored -> {DST}")
    print(f"  src/: {len(srcs)} .c/.h, 总 {total//1024//1024}MB")
    print(f"  prebuilt/linux-x86_64/: {len(libs)} libs")
    for l in sorted(libs):
        print(f"    {l.name} {l.stat().st_size//1024}KB")


if __name__ == "__main__":
    main()
