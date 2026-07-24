"""
视频处理操作 - 基于 ffmpeg 的音视频合并/统一转码/拼接/字幕烧录。

命令参数已在阶段0用真实 ffmpeg 全链路实测通过(含不同分辨率归一化、
concat 拼接、中文硬字幕、Windows 路径冒号转义)。
"""

import os
import re
from datetime import timedelta

from src.video_pipeline.ffmpeg_runtime import run_ffmpeg, probe_duration, probe_has_audio
from src.video_pipeline.text_utils import clean_text, is_too_short_caption, strict_chinese_caption

DEFAULT_TRANSITION_SECONDS = 0.22


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
    has_audio = probe_has_audio(in_path)
    args = ["-i", in_path]
    if not has_audio:
        args += ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo"]
    args += [
        "-vf", vf,
        "-map", "0:v:0", "-map", "0:a:0" if has_audio else "1:a:0",
        "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
        "-preset", "medium", "-crf", "20",
        "-c:a", "aac", "-ar", "48000", "-b:a", "128k", "-ac", "2",
        "-shortest",
        out_path,
    ]
    run_ffmpeg(args)


def concat_clips(clip_paths: list, filelist_path: str, out_path: str,
                 transition_seconds: float = DEFAULT_TRANSITION_SECONDS,
                 frame_rate: int = 24):
    """Join normalized clips with a short dissolve to suppress boundary flashes/repeated frames."""
    if len(clip_paths) == 1:
        run_ffmpeg(["-i", clip_paths[0], "-c", "copy", out_path])
        return
    durations = [probe_duration(path) for path in clip_paths]
    if any(duration <= transition_seconds * 2 for duration in durations):
        transition_seconds = 0.08
    with open(filelist_path, "w", encoding="utf-8") as f:
        for p in clip_paths:
            abspath = os.path.abspath(p).replace("\\", "/")
            f.write(f"file '{abspath}'\n")

    args = []
    for path in clip_paths:
        args += ["-i", path]
    filters = []
    video_label = "0:v"
    audio_label = "0:a"
    accumulated = durations[0]
    for index in range(1, len(clip_paths)):
        next_video = f"vx{index}"
        next_audio = f"ax{index}"
        offset = max(0.01, accumulated - transition_seconds)
        filters.append(
            f"[{video_label}][{index}:v]xfade=transition=fade:duration={transition_seconds:.3f}:"
            f"offset={offset:.3f}[{next_video}]"
        )
        filters.append(
            f"[{audio_label}][{index}:a]acrossfade=d={transition_seconds:.3f}:c1=tri:c2=tri[{next_audio}]"
        )
        accumulated += durations[index] - transition_seconds
        video_label = next_video
        audio_label = next_audio
    args += [
        "-filter_complex", ";".join(filters),
        "-map", f"[{video_label}]", "-map", f"[{audio_label}]",
        "-c:v", "libx264", "-profile:v", "high", "-pix_fmt", "yuv420p",
        "-preset", "medium", "-crf", "20",
        "-c:a", "aac", "-ar", "48000", "-b:a", "128k", "-ac", "2",
        out_path,
    ]
    run_ffmpeg(args)


def _escape_subtitles_path(path: str) -> str:
    """subtitles 滤镜里的路径需要把盘符冒号转义(E: -> E\\:)，反斜杠统一成正斜杠。"""
    return os.path.abspath(path).replace("\\", "/").replace(":", "\\:")


def burn_subtitles(in_path: str, srt_path: str, out_path: str):
    """硬字幕烧录(不可关闭，分享兼容性最好)。"""
    escaped = _escape_subtitles_path(srt_path)
    vf = (
        f"subtitles='{escaped}':charenc=UTF-8:force_style='FontName=Microsoft YaHei,"
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
        "-i", in_path,
        "-sub_charenc", "UTF-8",
        "-i", srt_path,
        "-map", "0", "-map", "1",
        "-c", "copy", "-c:s", "mov_text",
        "-metadata:s:s:0", "language=chi",
        out_path,
    ])


def write_plain_srt(text: str, duration: float, out_path: str):
    """Create a scene-level SRT when subtitles are needed without TTS timing."""
    text = strict_chinese_caption(text, max_len=120)
    duration = max(1.0, float(duration or 1.0))
    if not text:
        with open(out_path, "w", encoding="utf-8-sig") as f:
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
    with open(out_path, "w", encoding="utf-8-sig") as f:
        for idx, chunk in enumerate(chunks, 1):
            start = timedelta(seconds=per * (idx - 1))
            end = timedelta(seconds=per * idx)
            f.write(f"{idx}\n{_fmt_ts(start)} --> {_fmt_ts(end)}\n{chunk}\n\n")
    return out_path


def write_scripted_srt(lines: list[str], duration: float, out_path: str):
    """Create a scene-level SRT from structured narration/dialogue lines."""
    normalized = []
    for line in lines or []:
        text = strict_chinese_caption(line, max_len=120)
        if text:
            normalized.append(text)
    duration = max(1.0, float(duration or 1.0))
    if not normalized:
        with open(out_path, "w", encoding="utf-8-sig") as f:
            f.write("")
        return out_path

    chunks = []
    for line in normalized:
        rest = line
        while rest:
            chunk = rest[:28].rstrip("，。；、,. ")
            if not chunk:
                chunk = rest[:28]
            chunks.append(chunk)
            rest = rest[len(chunk):].lstrip("，。；、,. ")

    per = duration / max(1, len(chunks))
    with open(out_path, "w", encoding="utf-8-sig") as f:
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
        text = strict_chinese_caption("".join(text_lines), max_len=80)
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


def merge_srt(scene_srt_paths: list, scene_durations: list, out_path: str,
              transition_seconds: float = DEFAULT_TRANSITION_SECONDS):
    """把各场景独立 SRT(相对0开始)按累计真实时长偏移后合并成全片 SRT。

    scene_durations 用 normalized 片段的 ffprobe 实测时长，避免累计误差。
    某场景无字幕(路径为空或文件不存在)时跳过，但其时长仍计入后续偏移。
    """
    all_cues = []
    offset = timedelta(0)
    for index, (srt_path, dur) in enumerate(zip(scene_srt_paths, scene_durations)):
        if srt_path and os.path.exists(srt_path):
            try:
                with open(srt_path, "r", encoding="utf-8-sig") as f:
                    all_cues.extend(_compact_cues(_shift_srt(f.read(), offset)))
            except OSError:
                pass
        overlap = transition_seconds if index < len(scene_durations) - 1 else 0.0
        offset += timedelta(seconds=max(0.0, dur - overlap))

    with open(out_path, "w", encoding="utf-8-sig") as f:
        for i, (start, end, text) in enumerate(all_cues, 1):
            f.write(f"{i}\n{_fmt_ts(start)} --> {_fmt_ts(end)}\n{text}\n\n")
    return out_path
