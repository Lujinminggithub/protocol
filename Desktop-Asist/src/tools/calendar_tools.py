"""
日历日程工具 - 复用 calendar_store.py 的持久化逻辑，让 AI 能增/查日程。

date 参数要求绝对日期 yyyy-MM-dd；模型如果拿到的是"明天"这种相对表达，
应先调用 get_current_datetime 工具换算成绝对日期再传入。
"""

import json
from datetime import datetime

from src import calendar_store


def _validate_date(date_str: str):
    try:
        datetime.strptime(date_str, "%Y-%m-%d")
        return True
    except (ValueError, TypeError):
        return False


def _validate_time(time_str: str) -> str:
    if not time_str:
        return "00:00"
    try:
        datetime.strptime(time_str, "%H:%M")
        return time_str
    except ValueError:
        return "00:00"


def add_event(args: dict) -> str:
    date = str(args.get("date", "")).strip()
    text = str(args.get("text", "")).strip()
    if not _validate_date(date):
        return json.dumps(
            {"error": "date 格式不正确，必须是绝对日期 yyyy-MM-dd(可先调用 get_current_datetime 换算)。"},
            ensure_ascii=False,
        )
    if not text:
        return json.dumps({"error": "text(日程内容) 不能为空"}, ensure_ascii=False)

    time_str = _validate_time(str(args.get("time", "")).strip())
    reminder = bool(args.get("reminder", True))

    events = calendar_store.load()
    entry = {
        "id": calendar_store.new_id(),
        "text": text,
        "time": time_str,
        "reminder": reminder,
        "reminded": False,
    }
    events.setdefault(date, []).append(entry)
    calendar_store.save(events)

    return json.dumps(
        {"status": "ok", "date": date, "time": time_str, "text": text}, ensure_ascii=False
    )


def query_events(args: dict) -> str:
    date = str(args.get("date", "")).strip()
    if not _validate_date(date):
        return json.dumps(
            {"error": "date 格式不正确，必须是绝对日期 yyyy-MM-dd(可先调用 get_current_datetime 换算)。"},
            ensure_ascii=False,
        )
    events = calendar_store.load().get(date, [])
    return json.dumps(
        {
            "date": date,
            "count": len(events),
            "events": [{"time": e.get("time", ""), "text": e.get("text", "")} for e in events],
        },
        ensure_ascii=False,
    )
