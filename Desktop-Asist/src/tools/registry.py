"""
工具注册表 - 汇总全部可用工具，供 ChatWorker 的工具调用循环使用。
"""

import json

from src.logger import get_logger
from src.tools.base import ToolSpec
from src.tools import (
    weather,
    datetime_tool,
    calculator,
    file_search,
    calendar_tools,
    system_info,
    web_search,
)

TOOLS = {
    "get_weather": ToolSpec(
        name="get_weather",
        description="查询指定城市当前的实时天气(气温/湿度/天气状况/风速)。数据源 Open-Meteo，免费无需Key。",
        parameters={
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "城市名称，例如 南京、北京、东京"}
            },
            "required": ["city"],
        },
        func=weather.run,
    ),
    "get_current_datetime": ToolSpec(
        name="get_current_datetime",
        description=(
            "获取当前系统的绝对日期和时间。当用户提到“今天/明天/后天/上周”等相对日期表达时，"
            "应先调用本工具拿到当前绝对日期，再自行换算成 yyyy-MM-dd 格式使用。"
        ),
        parameters={"type": "object", "properties": {}, "required": []},
        func=datetime_tool.run,
    ),
    "calculate": ToolSpec(
        name="calculate",
        description="计算一个数学表达式，支持四则运算、括号、乘方，以及 sqrt/sin/cos/log 等常见函数。",
        parameters={
            "type": "object",
            "properties": {
                "expression": {"type": "string", "description": "数学表达式，例如 (3+4)*2 或 sqrt(16)"}
            },
            "required": ["expression"],
        },
        func=calculator.run,
    ),
    "search_files": ToolSpec(
        name="search_files",
        description="在用户主目录及其子目录内按文件名模式搜索文件(不读取文件内容)，返回匹配到的文件路径列表。",
        parameters={
            "type": "object",
            "properties": {
                "directory": {
                    "type": "string",
                    "description": "要搜索的目录，必须是用户主目录内的路径；留空则搜索整个主目录",
                },
                "pattern": {
                    "type": "string",
                    "description": "文件名匹配模式，支持通配符 * ?，例如 *.pdf 或 报告*",
                },
            },
            "required": ["pattern"],
        },
        func=file_search.run,
    ),
    "add_calendar_event": ToolSpec(
        name="add_calendar_event",
        description="向日历添加一条日程。date 必须是绝对日期 yyyy-MM-dd(如需相对日期请先调用 get_current_datetime 换算)。",
        parameters={
            "type": "object",
            "properties": {
                "date": {"type": "string", "description": "日程日期，格式 yyyy-MM-dd"},
                "text": {"type": "string", "description": "日程内容"},
                "time": {"type": "string", "description": "提醒时间，格式 HH:mm，不填默认 00:00"},
                "reminder": {"type": "boolean", "description": "是否到点弹出提醒，默认 true"},
            },
            "required": ["date", "text"],
        },
        func=calendar_tools.add_event,
    ),
    "query_calendar_events": ToolSpec(
        name="query_calendar_events",
        description="查询指定日期的所有日程。date 必须是绝对日期 yyyy-MM-dd。",
        parameters={
            "type": "object",
            "properties": {"date": {"type": "string", "description": "查询日期，格式 yyyy-MM-dd"}},
            "required": ["date"],
        },
        func=calendar_tools.query_events,
    ),
    "get_system_info": ToolSpec(
        name="get_system_info",
        description="获取本机当前 CPU/内存/磁盘占用等系统资源状态。",
        parameters={"type": "object", "properties": {}, "required": []},
        func=system_info.run,
    ),
    "web_search": ToolSpec(
        name="web_search",
        description="进行网页搜索，获取实时/最新信息(新闻、股价等)，返回搜索摘要与相关网页列表。",
        parameters={
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "搜索关键词"},
                "max_results": {"type": "integer", "description": "返回结果数量，1~10，默认5"},
            },
            "required": ["query"],
        },
        func=web_search.run,
        requires="tavily",
    ),
}


def get_tools_schema(cfg) -> list:
    """按当前配置过滤出可用工具的 OpenAI schema 列表(未配置对应Key的工具不报给模型)。"""
    out = []
    for spec in TOOLS.values():
        if spec.requires and not cfg.has_tool_key(spec.requires):
            continue
        out.append(spec.to_openai_schema())
    return out


def execute_tool(name: str, arguments: dict) -> str:
    """执行指定工具，统一兜底错误处理——工具异常绝不冒泡到调用方。"""
    spec = TOOLS.get(name)
    if spec is None:
        return json.dumps({"error": f"未知工具: {name}"}, ensure_ascii=False)
    try:
        return spec.func(arguments or {})
    except Exception as e:
        get_logger().exception(f"工具 {name} 执行异常")
        return json.dumps({"error": f"工具执行失败: {e}"}, ensure_ascii=False)
