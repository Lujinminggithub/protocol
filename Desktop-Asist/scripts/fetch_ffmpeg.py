"""构建前置脚本: 下载 ffmpeg essentials 静态版, 提取 ffmpeg.exe / ffprobe.exe 到 assets/ffmpeg/

用法: python scripts/fetch_ffmpeg.py
(打包 exe 前需要先跑一次, 二进制不提交进 git)
"""

import io
import os
import sys
import urllib.request
import zipfile

# gyan.dev 的固定别名 URL, 始终指向最新 release essentials 构建
URL = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
# 备用镜像(gyan.dev 访问不稳定时可切换): BtbN 的 GitHub release
FALLBACK_URL = (
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
    "ffmpeg-master-latest-win64-gpl.zip"
)

WANTED = ("ffmpeg.exe", "ffprobe.exe")


def _download(url: str) -> bytes:
    print(f"下载中: {url}")
    with urllib.request.urlopen(url, timeout=300) as resp:
        data = resp.read()
    print(f"下载完成: {len(data)} 字节")
    return data


def main():
    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets", "ffmpeg")
    os.makedirs(out_dir, exist_ok=True)

    try:
        data = _download(URL)
    except Exception as e:
        print(f"主源失败({e})，尝试备用源...")
        data = _download(FALLBACK_URL)

    print("解压中...")
    z = zipfile.ZipFile(io.BytesIO(data))
    extracted = []
    for name in z.namelist():
        base = os.path.basename(name)
        if base in WANTED:
            target = os.path.join(out_dir, base)
            with open(target, "wb") as f:
                f.write(z.read(name))
            extracted.append(base)
            print(f"  提取: {base} -> {target}")

    missing = set(WANTED) - set(extracted)
    if missing:
        print(f"警告: 未找到 {missing}，压缩包结构可能有变化")
        sys.exit(1)

    print(f"完成: {os.listdir(out_dir)}")


if __name__ == "__main__":
    main()
