"""
TTS 引擎 - edge-tts 封装(免费、无需API Key、微软Edge同款中文神经网络语音)。

同步入口供 QThread.run() 直接调用(内部 asyncio.run 在当前后台线程建独立事件循环)。
用 SubMaker 在合成语音时同步产出带时间戳的 SRT 字幕。
证书用显式注入 connector 的方式兜底 PyInstaller 打包环境下的 CA 问题。
"""

import asyncio
import os
import re
import ssl
from datetime import timedelta

import certifi
import edge_tts

from src.video_pipeline.ffmpeg_runtime import run_ffmpeg, probe_duration
from src.video_pipeline.text_utils import clean_text, is_too_short_caption, strict_chinese_caption

DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"

VOICE_CHOICES = [
    ("zh-CN-XiaoxiaoNeural", "晓晓 - 女声/新闻播报"),
    ("zh-CN-YunxiNeural", "云希 - 男声/阳光"),
    ("zh-CN-YunjianNeural", "云健 - 男声/纪录片解说"),
    ("zh-CN-XiaoyiNeural", "晓伊 - 女声/活泼"),
    ("zh-CN-YunyangNeural", "云扬 - 男声/新闻播报"),
    ("zh-CN-YunxiaNeural", "云夏 - 男声/少年可爱"),
    ("zh-CN-liaoning-XiaobeiNeural", "晓北 - 女声/东北幽默"),
    ("zh-CN-shaanxi-XiaoniNeural", "晓妮 - 女声/陕西明亮"),
    ("zh-HK-HiuGaaiNeural", "晓佳 - 粤语女声/亲切"),
    ("zh-HK-HiuMaanNeural", "晓曼 - 粤语女声/自然"),
    ("zh-HK-WanLungNeural", "云龙 - 粤语男声/稳重"),
    ("zh-TW-HsiaoChenNeural", "晓臻 - 台湾女声/自然"),
    ("zh-TW-HsiaoYuNeural", "晓雨 - 台湾女声/温和"),
    ("zh-TW-YunJheNeural", "云哲 - 台湾男声/自然"),
]


def _build_ssl_context() -> ssl.SSLContext:
    # 不依赖环境变量能否被 edge-tts 内部读取，直接基于 certifi 显式构造。
    # 打包环境下 SSL_CERT_FILE 已由 main.py 指向 _MEIPASS/certifi/cacert.pem。
    ca_path = os.environ.get("SSL_CERT_FILE") or certifi.where()
    return ssl.create_default_context(cafile=ca_path)


async def _synthesize_async(text: str, voice: str, mp3_path: str, srt_path: str):
    import aiohttp

    connector = aiohttp.TCPConnector(ssl=_build_ssl_context())
    # boundary="WordBoundary" 让字幕时间戳精确到词级，比默认句级更适合逐句字幕
    communicate = edge_tts.Communicate(
        text, voice, connector=connector, boundary="WordBoundary"
    )
    submaker = edge_tts.SubMaker()
    try:
        with open(mp3_path, "wb") as f:
            async for chunk in communicate.stream():
                if chunk["type"] == "audio":
                    f.write(chunk["data"])
                elif chunk["type"] == "WordBoundary":
                    submaker.feed(chunk)
    finally:
        await connector.close()

    with open(srt_path, "w", encoding="utf-8-sig") as f:
        f.write(_compact_srt(submaker.get_srt()))


async def _synthesize_audio_async(text: str, voice: str, mp3_path: str):
    import aiohttp

    connector = aiohttp.TCPConnector(ssl=_build_ssl_context())
    communicate = edge_tts.Communicate(text, voice, connector=connector)
    try:
        with open(mp3_path, "wb") as f:
            async for chunk in communicate.stream():
                if chunk["type"] == "audio":
                    f.write(chunk["data"])
    finally:
        await connector.close()


def _parse_srt_blocks(srt_text: str) -> list:
    blocks = []
    for block in re.split(r"\n\s*\n", (srt_text or "").strip()):
        lines = [ln.rstrip() for ln in block.splitlines() if ln.strip()]
        if len(lines) < 3:
            continue
        time_line = lines[1]
        text = "".join(lines[2:]).strip()
        if not text:
            continue
        blocks.append((time_line, clean_text(text, max_len=80)))
    return blocks


def _compact_srt(srt_text: str) -> str:
    blocks = _parse_srt_blocks(srt_text)
    if not blocks:
        return ""

    merged = []
    current_time = None
    current_text = []
    for time_line, text in blocks:
        if not text:
            continue
        if current_time is None:
            current_time = time_line
            current_text = [text]
            continue
        joined = "".join(current_text)
        should_merge = (
            len(joined) < 12
            or is_too_short_caption(text)
            or not re.search(r"[，。！？；：,.!?;:]$", joined)
        )
        if should_merge and len(joined + text) <= 28:
            current_time = f"{current_time.split(' --> ')[0]} --> {time_line.split(' --> ')[1]}"
            current_text.append(text)
            continue
        merged.append((current_time, clean_text("".join(current_text), max_len=28)))
        current_time = time_line
        current_text = [text]

    if current_time and current_text:
        merged.append((current_time, clean_text("".join(current_text), max_len=28)))

    lines = []
    for idx, (time_line, text) in enumerate(merged, 1):
        if is_too_short_caption(text, min_chars=3):
            continue
        lines.append(f"{idx}\n{time_line}\n{text}")
    return "\n\n".join(lines).strip() + ("\n" if lines else "")


def _fmt_ts(td: timedelta) -> str:
    total_ms = int(td.total_seconds() * 1000)
    h, rem = divmod(total_ms, 3600_000)
    m, rem = divmod(rem, 60_000)
    s, ms = divmod(rem, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def _write_scene_srt(entries: list[tuple[str, float]], srt_path: str):
    lines = []
    offset = timedelta(0)
    cue_index = 1
    for subtitle_text, duration in entries:
        subtitle_text = strict_chinese_caption(subtitle_text, max_len=120)
        duration = max(0.05, float(duration or 0.0))
        if not subtitle_text:
            offset += timedelta(seconds=duration)
            continue
        start = offset
        end = offset + timedelta(seconds=duration)
        lines.append(f"{cue_index}\n{_fmt_ts(start)} --> {_fmt_ts(end)}\n{subtitle_text}\n")
        cue_index += 1
        offset = end
    with open(srt_path, "w", encoding="utf-8-sig") as f:
        f.write("\n".join(lines))


def synthesize(text: str, voice: str, mp3_path: str, srt_path: str):
    """同步入口：文本 -> mp3 音频 + srt 字幕。供 QThread.run() 内直接调用。"""
    text = clean_text(text, max_len=500)
    if not text:
        # 空旁白：写一个极短静音占位不现实，交给上层用静音处理；这里直接抛
        raise ValueError("旁白文本为空")
    asyncio.run(_synthesize_async(text, voice or DEFAULT_VOICE, mp3_path, srt_path))


def synthesize_segments(segments: list[dict], voice_fallback: str, mp3_path: str, srt_path: str, temp_dir: str):
    """Synthesize multiple narration/dialogue segments with per-segment voices."""
    normalized_segments = []
    for idx, segment in enumerate(segments or []):
        speech_text = clean_text(segment.get("speech_text", ""), max_len=240)
        subtitle_text = strict_chinese_caption(
            segment.get("subtitle_text", "") or speech_text, max_len=240
        )
        voice = clean_text(segment.get("voice", "") or voice_fallback, max_len=60)
        if speech_text:
            normalized_segments.append(
                {
                    "index": idx,
                    "speech_text": speech_text,
                    "subtitle_text": subtitle_text,
                    "voice": voice or DEFAULT_VOICE,
                }
            )
    if not normalized_segments:
        raise ValueError("语音片段为空")

    os.makedirs(temp_dir, exist_ok=True)
    filelist_path = os.path.join(temp_dir, "tts_segments.txt")
    audio_entries = []
    srt_entries = []
    for segment in normalized_segments:
        seg_path = os.path.join(temp_dir, f"seg_{segment['index']:02d}.mp3")
        asyncio.run(_synthesize_audio_async(segment["speech_text"], segment["voice"], seg_path))
        duration = probe_duration(seg_path)
        audio_entries.append(seg_path)
        srt_entries.append((segment["subtitle_text"], duration))

    with open(filelist_path, "w", encoding="utf-8") as f:
        for path in audio_entries:
            f.write(f"file '{os.path.abspath(path).replace('\\', '/')}'\n")

    run_ffmpeg([
        "-f", "concat", "-safe", "0", "-i", filelist_path,
        "-c:a", "libmp3lame", "-b:a", "128k",
        mp3_path,
    ])
    _write_scene_srt(srt_entries, srt_path)


def synthesize_preview_wav(text: str, voice: str, wav_path: str, temp_dir: str):
    """Create a short preview wav for local playback."""
    preview_text = clean_text(text, max_len=120)
    if not preview_text:
        raise ValueError("试听文本为空")
    os.makedirs(temp_dir, exist_ok=True)
    mp3_path = os.path.join(temp_dir, "preview.mp3")
    asyncio.run(_synthesize_audio_async(preview_text, voice or DEFAULT_VOICE, mp3_path))
    run_ffmpeg([
        "-i", mp3_path,
        "-ac", "2",
        "-ar", "48000",
        "-c:a", "pcm_s16le",
        wav_path,
    ])
    return wav_path
