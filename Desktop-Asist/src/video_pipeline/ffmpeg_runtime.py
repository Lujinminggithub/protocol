"""
ffmpeg 运行时封装 - 定位内置的 ffmpeg.exe / ffprobe.exe 并提供调用封装。
"""

import subprocess

from src.paths import resource_path
from src.video_pipeline.base import PipelineError

# GUI 无控制台程序调用子进程时防止一闪而过的黑窗
CREATE_NO_WINDOW = 0x08000000


def ffmpeg_path() -> str:
    return resource_path("assets", "ffmpeg", "ffmpeg.exe")


def ffprobe_path() -> str:
    return resource_path("assets", "ffmpeg", "ffprobe.exe")


def run_ffmpeg(args: list, timeout: int = None) -> subprocess.CompletedProcess:
    """执行一次 ffmpeg 命令(阻塞)，失败抛 PipelineError(带 stderr 摘要)。"""
    cmd = [ffmpeg_path(), "-y", "-hide_banner", "-loglevel", "error", *args]
    try:
        r = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            creationflags=CREATE_NO_WINDOW,
        )
    except FileNotFoundError:
        raise PipelineError("找不到内置的 ffmpeg.exe，打包/资源可能损坏。")
    except subprocess.TimeoutExpired:
        raise PipelineError("ffmpeg 处理超时。")
    if r.returncode != 0:
        raise PipelineError(f"ffmpeg 执行失败: {(r.stderr or '')[-300:]}")
    return r


def probe_duration(path: str) -> float:
    """用 ffprobe 探测媒体文件时长(秒)，失败返回 0.0。"""
    cmd = [
        ffprobe_path(),
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        path,
    ]
    try:
        r = subprocess.run(
            cmd, capture_output=True, text=True, creationflags=CREATE_NO_WINDOW
        )
        return float((r.stdout or "").strip())
    except (ValueError, FileNotFoundError, subprocess.SubprocessError):
        return 0.0
