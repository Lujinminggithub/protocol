"""
当前日期时间工具 - 纯本地计算，无网络请求。

大模型的知识存在训练截止日期，不知道"现在"是哪一天；用户提到"今天/明天/
上周"等相对日期时，应先调用本工具拿到绝对日期再自行换算。
"""

import json
from datetime import datetime

_WEEKDAY_CN = ["星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日"]


def run(args: dict) -> str:
    now = datetime.now()
    return json.dumps(
        {
            "date": now.strftime("%Y-%m-%d"),
            "time": now.strftime("%H:%M:%S"),
            "weekday": _WEEKDAY_CN[now.weekday()],
            "iso": now.isoformat(timespec="seconds"),
        },
        ensure_ascii=False,
    )
