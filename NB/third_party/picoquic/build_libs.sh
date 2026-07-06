#!/bin/bash
# 从 vendored 源码按目标平台构建 picoquic 静态库 -> prebuilt/<platform>/
#
# 复现 picoquic 原生两阶段构建(顶层 CMake 用 find_package(PTLS), 需 picotls 先构建并传 PTLS_* 变量):
#   1) picotls: cmake+make -> libpicotls-{core,minicrypto,openssl}.a
#   2) picoquic: cmake(传 PTLS_*)+make -> libpicoquic-{core,log}.a
# 产出 5 个 .a 汇集到 prebuilt/<platform>/。跨平台: 换机器直接重跑即可。
#
# 用法: bash build_libs.sh [platform]   (默认 <os>-<arch>, 如 linux-x86_64)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/src"
PLATFORM=${1:-$(uname -s | tr 'A-Z' 'a-z')-$(uname -m)}
OUT="$HERE/prebuilt/$PLATFORM"
BD="build-$PLATFORM"        # 各平台独立 build 目录(不入 git)
JOBS=$(nproc 2>/dev/null || echo 4)

echo "[build_libs] platform=$PLATFORM src=$SRC out=$OUT"
mkdir -p "$OUT"

# 1) picotls
echo "[1/2] picotls ..."
cd "$SRC/picotls"
cmake -S . -B "$BD" -DCMAKE_BUILD_TYPE=Release -DWITH_FUSION=OFF >/dev/null
cmake --build "$BD" --target picotls-core picotls-minicrypto picotls-openssl -j"$JOBS" >/dev/null
PTLS_INC="$SRC/picotls/include"
PTLS_CORE="$SRC/picotls/$BD/libpicotls-core.a"
PTLS_MINI="$SRC/picotls/$BD/libpicotls-minicrypto.a"
PTLS_SSL="$SRC/picotls/$BD/libpicotls-openssl.a"

# 2) picoquic (find_package(PTLS) 靠传入的 PTLS_* 变量)
echo "[2/2] picoquic ..."
cd "$SRC"
cmake -S . -B "$BD" -DCMAKE_BUILD_TYPE=Release \
  -DPICOQUIC_FETCH_PTLS=OFF \
  -DPTLS_INCLUDE_DIR="$PTLS_INC" \
  -DPTLS_CORE_LIBRARY="$PTLS_CORE" \
  -DPTLS_OPENSSL_LIBRARY="$PTLS_SSL" \
  -DPTLS_MINICRYPTO_LIBRARY="$PTLS_MINI" >/dev/null
cmake --build "$BD" --target picoquic-core picoquic-log -j"$JOBS" >/dev/null

# 3) 汇集 .a
cp "$SRC/$BD/libpicoquic-core.a" "$SRC/$BD/libpicoquic-log.a" "$OUT/"
cp "$PTLS_CORE" "$PTLS_MINI" "$PTLS_SSL" "$OUT/"
echo "[build_libs] done: $(ls "$OUT"/*.a | wc -l) libs -> $OUT"
ls -l "$OUT"/*.a
