"""Text cleanup helpers for prompts, narration, and subtitles."""

import re


_MOJIBAKE_MARKERS = (
    "锛", "銆", "鐨", "涓", "绋", "闂", "鍦", "瑙", "戠", "�",
    "掑", "畾", "濡", "嗙", "収", "閫", "鍜", "姝", "瀹", "搴",
    "Ã", "Â", "â€", "å", "æ", "ç", "????",
)


def looks_mojibake(text: str) -> bool:
    if not text:
        return False
    hits = sum(text.count(marker) for marker in _MOJIBAKE_MARKERS)
    return hits >= 1 or "�" in text


def unrepaired_mojibake_score(text: str) -> int:
    if not text:
        return 0
    return sum(text.count(marker) for marker in _MOJIBAKE_MARKERS) + text.count("?")


def repair_mojibake(text: str) -> str:
    """Repair common UTF-8 text that was accidentally decoded as GBK/Latin-1."""
    if not text or not looks_mojibake(text):
        return text or ""

    candidates = [text]
    for src in ("gbk", "latin1"):
        try:
            candidates.append(text.encode(src, errors="strict").decode("utf-8", errors="strict"))
        except UnicodeError:
            pass

    def score(value: str) -> int:
        marker_penalty = sum(value.count(marker) for marker in _MOJIBAKE_MARKERS) * 3
        cjk_bonus = len(re.findall(r"[\u4e00-\u9fff]", value))
        replacement_penalty = value.count("�") * 10
        return cjk_bonus - marker_penalty - replacement_penalty

    return max(candidates, key=score)


def clean_text(text: str, *, max_len: int = None) -> str:
    value = repair_mojibake(str(text or ""))
    value = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", "", value)
    value = re.sub(r"\s+", " ", value).strip()
    if max_len and len(value) > max_len:
        value = value[:max_len].rstrip("，。；、,. ")
    return value


def is_too_short_caption(text: str, min_chars: int = 4) -> bool:
    value = re.sub(r"\s+", "", text or "")
    return 0 < len(value) < min_chars


def strict_chinese_caption(text: str, max_len: int = 120) -> str:
    """Return display-safe Chinese subtitle text, rejecting unresolved mojibake/English output."""
    value = clean_text(text, max_len=max_len)
    replacements = {
        "WiFi": "无线网络",
        "WIFI": "无线网络",
        "wifi": "无线网络",
        "APP": "应用",
        "App": "应用",
        "AI": "人工智能",
    }
    for source, target in replacements.items():
        value = value.replace(source, target)
    rare_hits = sum(value.count(marker) for marker in (
        "锛", "銆", "鐨", "涓", "闂", "瑙", "戠", "掑", "嗙", "閫",
        "杩", "欐", "槸", "娈", "鐮", "Ã", "Â", "â€", "�",
    ))
    if "�" in value or rare_hits >= 2:
        return ""
    value = re.sub(r"[A-Za-z][A-Za-z0-9_'’-]*", "", value)
    value = re.sub(r"\s+", " ", value).strip()
    if not re.search(r"[\u3400-\u9fff]", value):
        return ""
    return clean_text(value, max_len=max_len)
