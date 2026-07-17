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

import certifi
import edge_tts

from src.video_pipeline.text_utils import clean_text, is_too_short_caption

DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"

VOICE_CHOICES = [
    ("zh-CN-XiaoxiaoNeural", "晓晓 - 女声/新闻播报"),
    ("zh-CN-YunxiNeural", "云希 - 男声/阳光"),
    ("zh-CN-YunjianNeural", "云健 - 男声/纪录片解说"),
    ("zh-CN-XiaoyiNeural", "晓伊 - 女声/活泼"),
    ("zh-CN-YunyangNeural", "云扬 - 男声/新闻播报"),
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

    with open(srt_path, "w", encoding="utf-8") as f:
        f.write(_compact_srt(submaker.get_srt()))


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


def synthesize(text: str, voice: str, mp3_path: str, srt_path: str):
    """同步入口：文本 -> mp3 音频 + srt 字幕。供 QThread.run() 内直接调用。"""
    text = clean_text(text, max_len=500)
    if not text:
        # 空旁白：写一个极短静音占位不现实，交给上层用静音处理；这里直接抛
        raise ValueError("旁白文本为空")
    asyncio.run(_synthesize_async(text, voice or DEFAULT_VOICE, mp3_path, srt_path))
