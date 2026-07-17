"""
日历数据持久化 - 按日期分组存储于 JSON，原子写避免半写损坏文件。

数据结构: {"yyyy-MM-dd": [{"id":..., "text":..., "time":..., "reminder":..., "reminded":...}]}
"""

import json
import os
import uuid

from src.paths import CALENDAR_FILE, ensure_user_data_dir


def load() -> dict:
    """加载全部日程数据。"""
    if not os.path.exists(CALENDAR_FILE):
        return {}
    try:
        with open(CALENDAR_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except (json.JSONDecodeError, IOError):
        return {}


def save(events: dict):
    """保存全部日程数据(原子写)。"""
    ensure_user_data_dir()
    tmp = CALENDAR_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(events, f, ensure_ascii=False, indent=2)
    os.replace(tmp, CALENDAR_FILE)


def new_id() -> str:
    return uuid.uuid4().hex
