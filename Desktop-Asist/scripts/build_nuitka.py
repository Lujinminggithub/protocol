r"""Build AISprite with Nuitka.

Usage:
    .venv_build\Scripts\python.exe scripts\build_nuitka.py
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import imageio_ffmpeg

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DIST_DIR = PROJECT_ROOT / "dist_nuitka"
BUILD_DIR = PROJECT_ROOT / "build_nuitka"
MAIN_FILE = PROJECT_ROOT / "main.py"
ICON_FILE = PROJECT_ROOT / "assets" / "icons" / "sprite.ico"
FFMPEG_DIR = PROJECT_ROOT / "assets" / "ffmpeg"


def _which(name: str) -> str | None:
    return shutil.which(name)


def _compiler_args() -> list[str]:
    if _which("cl"):
        return ["--msvc=latest"]
    if _which("clang"):
        return ["--clang"]
    if _which("gcc"):
        return []
    # Nuitka can download MinGW toolchain on Windows when allowed.
    return ["--mingw64", "--assume-yes-for-downloads"]


def _ensure_ffmpeg_assets():
    FFMPEG_DIR.mkdir(parents=True, exist_ok=True)
    ffmpeg_target = FFMPEG_DIR / "ffmpeg.exe"
    ffprobe_target = FFMPEG_DIR / "ffprobe.exe"

    if not ffmpeg_target.exists():
        source = Path(imageio_ffmpeg.get_ffmpeg_exe())
        print(f"补齐 ffmpeg 资源: {source} -> {ffmpeg_target}")
        shutil.copy2(source, ffmpeg_target)

    if not ffprobe_target.exists():
        # 当前项目运行时已经支持 ffprobe 缺失时回退到 ffmpeg 探测时长；
        # 这里提供同源可执行文件占位，保证打包资源完整一致。
        print(f"补齐 ffprobe 兼容占位: {ffmpeg_target} -> {ffprobe_target}")
        shutil.copy2(ffmpeg_target, ffprobe_target)


def build():
    _ensure_ffmpeg_assets()
    DIST_DIR.mkdir(exist_ok=True)
    BUILD_DIR.mkdir(exist_ok=True)
    ffmpeg_file = FFMPEG_DIR / "ffmpeg.exe"
    ffprobe_file = FFMPEG_DIR / "ffprobe.exe"

    command = [
        sys.executable,
        "-m",
        "nuitka",
        str(MAIN_FILE),
        "--onefile",
        "--onefile-no-compression",
        "--assume-yes-for-downloads",
        "--enable-plugin=pyqt6",
        "--windows-console-mode=disable",
        f"--windows-icon-from-ico={ICON_FILE}",
        "--output-filename=AISprite.exe",
        f"--output-dir={DIST_DIR}",
        f"--remove-output",
        f"--include-data-dir={PROJECT_ROOT / 'assets' / 'icons'}=assets/icons",
        f"--include-data-file={ffmpeg_file}=assets/ffmpeg/ffmpeg.exe",
        f"--include-data-file={ffprobe_file}=assets/ffmpeg/ffprobe.exe",
        f"--include-package-data=certifi",
        f"--include-package-data=imageio_ffmpeg",
        "--nofollow-import-to=tkinter,pytest,setuptools",
        "--onefile-tempdir-spec={CACHE_DIR}/AISprite-Onefile",
    ]
    command.extend(_compiler_args())

    print("Running Nuitka build:")
    print(" ".join(str(part) for part in command))
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)


if __name__ == "__main__":
    build()
