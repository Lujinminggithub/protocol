"""
聊天记录持久化 - JSON 落盘，原子写避免半写损坏文件。
"""

import json
import os

from src.paths import CHAT_HISTORY_FILE, ensure_user_data_dir

_MAX_KEEP = 200


def load_history(limit: int = _MAX_KEEP) -> list:
    """加载聊天历史(只包含 role/content，不含 reasoning_content)。"""
    if not os.path.exists(CHAT_HISTORY_FILE):
        return []
    try:
        with open(CHAT_HISTORY_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data[-limit:]
    except (json.JSONDecodeError, IOError):
        return []


def save_history(history: list):
    """保存聊天历史，原子写(先写临时文件再替换)避免中途崩溃损坏文件。"""
    ensure_user_data_dir()
    tmp = CHAT_HISTORY_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(history[-_MAX_KEEP:], f, ensure_ascii=False, indent=2)
    os.replace(tmp, CHAT_HISTORY_FILE)


def clear_history():
    """清空聊天历史文件。"""
    if os.path.exists(CHAT_HISTORY_FILE):
        try:
            os.remove(CHAT_HISTORY_FILE)
        except OSError:
            pass
