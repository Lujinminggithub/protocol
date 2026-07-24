"""
Runtime helpers for bundled ffmpeg / ffprobe binaries.
"""

import os
import re
import subprocess
from pathlib import Path

try:
    import imageio_ffmpeg
except Exception:  # pragma: no cover - optional fallback import
    imageio_ffmpeg = None

from src.paths import resource_path
from src.video_pipeline.base import PipelineError

CREATE_NO_WINDOW = 0x08000000
_DURATION_RE = re.compile(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)")


def ffmpeg_path() -> str:
    bundled = resource_path("assets", "ffmpeg", "ffmpeg.exe")
    if os.path.exists(bundled):
        return bundled
    if imageio_ffmpeg is not None:
        try:
            fallback = imageio_ffmpeg.get_ffmpeg_exe()
            if fallback and os.path.exists(fallback):
                return fallback
        except Exception:
            pass
    return bundled


def ffprobe_path() -> str:
    bundled = resource_path("assets", "ffmpeg", "ffprobe.exe")
    return bundled if os.path.exists(bundled) else ""


def run_ffmpeg(args: list, timeout: int = None) -> subprocess.CompletedProcess:
    """Execute ffmpeg and raise a user-readable PipelineError on failure."""
    cmd = [ffmpeg_path(), "-y", "-hide_banner", "-loglevel", "error", *args]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            creationflags=CREATE_NO_WINDOW,
        )
    except FileNotFoundError:
        raise PipelineError("找不到内置的 ffmpeg.exe，打包资源可能损坏。")
    except subprocess.TimeoutExpired:
        raise PipelineError("ffmpeg 处理超时。")
    if result.returncode != 0:
        raise PipelineError(f"ffmpeg 执行失败: {(result.stderr or '')[-300:]}")
    return result


def _probe_with_ffprobe(path: str) -> float:
    probe_exe = ffprobe_path()
    if not probe_exe:
        raise FileNotFoundError("ffprobe not available")
    cmd = [
        probe_exe,
        "-v", "error",
        "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1",
        path,
    ]
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        creationflags=CREATE_NO_WINDOW,
    )
    return float((result.stdout or "").strip())


def _probe_with_ffmpeg(path: str) -> float:
    cmd = [ffmpeg_path(), "-i", path]
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        creationflags=CREATE_NO_WINDOW,
    )
    output = f"{result.stdout or ''}\n{result.stderr or ''}"
    match = _DURATION_RE.search(output)
    if not match:
        return 0.0
    hours = int(match.group(1))
    minutes = int(match.group(2))
    seconds = float(match.group(3))
    return hours * 3600 + minutes * 60 + seconds


def probe_duration(path: str) -> float:
    """Probe media duration in seconds. Falls back to ffmpeg if ffprobe is unavailable."""
    if not path or not os.path.exists(path):
        return 0.0
    try:
        return _probe_with_ffprobe(path)
    except (ValueError, FileNotFoundError, subprocess.SubprocessError):
        try:
            return _probe_with_ffmpeg(path)
        except (ValueError, FileNotFoundError, subprocess.SubprocessError):
            return 0.0


def probe_has_audio(path: str) -> bool:
    if not path or not os.path.exists(path):
        return False
    cmd = [ffmpeg_path(), "-hide_banner", "-i", path]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            creationflags=CREATE_NO_WINDOW,
        )
    except (FileNotFoundError, subprocess.SubprocessError):
        return False
    return "Audio:" in f"{result.stdout or ''}\n{result.stderr or ''}"
