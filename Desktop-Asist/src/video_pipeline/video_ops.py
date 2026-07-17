"""
视频处理操作 - 基于 ffmpeg 的音视频合并/统一转码/拼接/字幕烧录。

命令参数已在阶段0用真实 ffmpeg 全链路实测通过(含不同分辨率归一化、
concat 拼接、中文硬字幕、Windows 路径冒号转义)。
"""

import os
import re
from datetime import timedelta

from src.video_pipeline.ffmpeg_runtime import run_ffmpeg, probe_duration
from src.video_pipeline.text_utils import clean_text, is_too_short_caption


def merge_audio_video(video_path: str, audio_path: str, out_path: str):
    """把旁白音频叠加到视频上。视频通常比旁白长，音频结束后是静音，不截断画面。"""
    run_ffmpeg([
        "-i", video_path,
        "-i", audio_path,
        "-map", "0:v:0", "-map", "1:a:0",
        "-c:v", "copy", "-c:a", "aac", "-b:a", "128k",
        out_path,
    ])


def trim_clip_start(in_path: str, out_path: str, start_seconds: float):
    """裁掉视频开头 start_seconds 秒(去除 image-to-video 起始定妆照定格段)。

    只保留视频流(-an)，因为 raw 视频的音轨用不上(旁白是后面单独合成的)。
    -ss 放在 -i 之后是精确到帧的裁剪。
    """
    run_ffmpeg([
        "-i", in_path,
        "-ss", f"{start_seconds}",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
        "-an",
        out_path,
    ])


def normalize_clip(in_path: str, out_path: str, width: int, height: int, frame_rate: int):
    """统一转码到目标规格，保证后续 concat 拼接不因分辨率/编码差异失败。"""
    vf = (
        f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
        f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,fps={frame_rate}"
    )
    run_ffmpeg([
        "-i", in_path,
        "-vf", vf,
        "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
        "-preset", "medium", "-crf", "20",
        "-c:a", "aac", "-ar", "48000", "-b:a", "128k", "-ac", "2",
        out_path,
    ])


def make_placeholder_clip(out_path: str, width: int, height: int, frame_rate: int,
                          seconds: float, audio_path: str = None, image_path: str = None):
    """场景失败时的占位片段：有关键帧图用图定格，否则纯黑屏；音频用旁白或静音。

    产物走的规格与 normalize_clip 一致，能直接进 concat。
    """
    seconds = max(1.0, seconds)
    args = ["-y"] if False else []  # run_ffmpeg 已带 -y
    if image_path and os.path.exists(image_path):
        args += ["-loop", "1", "-i", image_path]
    else:
        args += ["-f", "lavfi", "-i", f"color=c=black:s={width}x{height}:r={frame_rate}"]
    if audio_path and os.path.exists(audio_path):
        args += ["-i", audio_path]
    else:
        args += ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo"]
    vf = (
        f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
        f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,fps={frame_rate}"
    )
    args += [
        "-t", f"{seconds}",
        "-vf", vf,
        "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
        "-preset", "medium", "-crf", "20",
        "-c:a", "aac", "-ar", "48000", "-b:a", "128k", "-ac", "2",
        "-shortest",
        out_path,
    ]
    run_ffmpeg(args)


def concat_clips(clip_paths: list, filelist_path: str, out_path: str):
    """用 concat demuxer 顺序拼接(所有片段已统一转码为同规格，纯 stream copy)。"""
    with open(filelist_path, "w", encoding="utf-8") as f:
        for p in clip_paths:
            abspath = os.path.abspath(p).replace("\\", "/")
            f.write(f"file '{abspath}'\n")
    run_ffmpeg([
        "-f", "concat", "-safe", "0", "-i", filelist_path,
        "-c", "copy",
        out_path,
    ])


def _escape_subtitles_path(path: str) -> str:
    """subtitles 滤镜里的路径需要把盘符冒号转义(E: -> E\\:)，反斜杠统一成正斜杠。"""
    return os.path.abspath(path).replace("\\", "/").replace(":", "\\:")


def burn_subtitles(in_path: str, srt_path: str, out_path: str):
    """硬字幕烧录(不可关闭，分享兼容性最好)。"""
    escaped = _escape_subtitles_path(srt_path)
    vf = (
        f"subtitles='{escaped}':force_style='FontName=Microsoft YaHei,"
        "FontSize=20,PrimaryColour=&HFFFFFF&,OutlineColour=&H000000&,"
        "BorderStyle=1,Outline=1,Shadow=0'"
    )
    run_ffmpeg([
        "-i", in_path,
        "-vf", vf,
        "-c:v", "libx264", "-preset", "medium", "-crf", "20",
        "-c:a", "copy",
        out_path,
    ])


def mux_soft_subtitles(in_path: str, srt_path: str, out_path: str):
    """软字幕(封装成 mov_text 轨道，播放器可开关，但兼容性不如硬字幕)。"""
    run_ffmpeg([
        "-i", in_path, "-i", srt_path,
        "-map", "0", "-map", "1",
        "-c", "copy", "-c:s", "mov_text",
        "-metadata:s:s:0", "language=chi",
        out_path,
    ])


def write_plain_srt(text: str, duration: float, out_path: str):
    """Create a scene-level SRT when subtitles are needed without TTS timing."""
    text = clean_text(text, max_len=120)
    duration = max(1.0, float(duration or 1.0))
    if not text:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write("")
        return out_path

    chunks = []
    while text:
        chunk = text[:28].rstrip("，。；、,. ")
        if not chunk:
            chunk = text[:28]
        chunks.append(chunk)
        text = text[len(chunk):].lstrip("，。；、,. ")

    per = duration / max(1, len(chunks))
    with open(out_path, "w", encoding="utf-8") as f:
        for idx, chunk in enumerate(chunks, 1):
            start = timedelta(seconds=per * (idx - 1))
            end = timedelta(seconds=per * idx)
            f.write(f"{idx}\n{_fmt_ts(start)} --> {_fmt_ts(end)}\n{chunk}\n\n")
    return out_path


# ===== SRT 时间轴合并 =====

_SRT_TIME_RE = re.compile(
    r"(\d{2}):(\d{2}):(\d{2})[,.](\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2})[,.](\d{3})"
)


def _parse_ts(h, m, s, ms) -> timedelta:
    return timedelta(hours=int(h), minutes=int(m), seconds=int(s), milliseconds=int(ms))


def _fmt_ts(td: timedelta) -> str:
    total_ms = int(td.total_seconds() * 1000)
    h, rem = divmod(total_ms, 3600_000)
    m, rem = divmod(rem, 60_000)
    s, ms = divmod(rem, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def _shift_srt(srt_text: str, offset: timedelta) -> list:
    """把一段 SRT 的所有时间戳整体偏移 offset，返回 (start, end, text) 三元组列表。"""
    blocks = re.split(r"\n\s*\n", srt_text.strip())
    cues = []
    for block in blocks:
        m = _SRT_TIME_RE.search(block)
        if not m:
            continue
        start = _parse_ts(*m.group(1, 2, 3, 4)) + offset
        end = _parse_ts(*m.group(5, 6, 7, 8)) + offset
        # 时间行之后的都是字幕文本
        lines = block.splitlines()
        text_lines = []
        seen_time = False
        for ln in lines:
            if _SRT_TIME_RE.search(ln):
                seen_time = True
                continue
            if seen_time:
                text_lines.append(ln)
        text = clean_text("".join(text_lines), max_len=80)
        if text:
            cues.append((start, end, text))
    return cues


def _compact_cues(cues: list, max_chars: int = 28, min_chars: int = 4) -> list:
    compacted = []
    cur_start = None
    cur_end = None
    cur_text = ""
    for start, end, text in cues:
        text = clean_text(text, max_len=max_chars)
        if not text:
            continue
        if cur_start is None:
            cur_start, cur_end, cur_text = start, end, text
            continue
        should_merge = (
            len(cur_text) < 12
            or is_too_short_caption(text, min_chars)
            or not re.search(r"[，。！？；：,.!?;:]$", cur_text)
        )
        if should_merge and len(cur_text + text) <= max_chars:
            cur_end = end
            cur_text = clean_text(cur_text + text, max_len=max_chars)
            continue
        if not is_too_short_caption(cur_text, min_chars=3):
            compacted.append((cur_start, cur_end, cur_text))
        cur_start, cur_end, cur_text = start, end, text

    if cur_start is not None and not is_too_short_caption(cur_text, min_chars=3):
        compacted.append((cur_start, cur_end, cur_text))
    return compacted


def merge_srt(scene_srt_paths: list, scene_durations: list, out_path: str):
    """把各场景独立 SRT(相对0开始)按累计真实时长偏移后合并成全片 SRT。

    scene_durations 用 normalized 片段的 ffprobe 实测时长，避免累计误差。
    某场景无字幕(路径为空或文件不存在)时跳过，但其时长仍计入后续偏移。
    """
    all_cues = []
    offset = timedelta(0)
    for srt_path, dur in zip(scene_srt_paths, scene_durations):
        if srt_path and os.path.exists(srt_path):
            try:
                with open(srt_path, "r", encoding="utf-8") as f:
                    all_cues.extend(_compact_cues(_shift_srt(f.read(), offset)))
            except OSError:
                pass
        offset += timedelta(seconds=dur)

    with open(out_path, "w", encoding="utf-8") as f:
        for i, (start, end, text) in enumerate(all_cues, 1):
            f.write(f"{i}\n{_fmt_ts(start)} --> {_fmt_ts(end)}\n{text}\n\n")
    return out_path
