"""
网页搜索工具 - Tavily API(需要用户自行申请免费 Key，与 Agnes/DeepSeek 的 Key 无关)。

DeepSeek/Agnes 都只提供标准 Function Calling 协议本身，不提供服务端托管的
联网搜索能力，因此"搜索"这个动作必须由客户端接一个真正的搜索数据源来执行。
"""

import json

import requests

from src.config import get_config

_SEARCH_URL = "https://api.tavily.com/search"


def run(args: dict) -> str:
    query = str(args.get("query", "")).strip()
    if not query:
        return json.dumps({"error": "query 不能为空"}, ensure_ascii=False)

    max_results = args.get("max_results", 5)
    try:
        max_results = min(max(int(max_results), 1), 10)
    except (TypeError, ValueError):
        max_results = 5

    cfg = get_config()
    api_key = cfg.get_tool_settings("tavily").get("api_key", "")
    if not api_key:
        return json.dumps({"error": "未配置 Tavily API Key，无法使用网页搜索。"}, ensure_ascii=False)

    try:
        resp = requests.post(
            _SEARCH_URL,
            headers={"Authorization": f"Bearer {api_key}"},
            json={"query": query, "max_results": max_results, "include_answer": True},
            timeout=15,
        )
        resp.raise_for_status()
        data = resp.json()
        results = [
            {
                "title": r.get("title"),
                "url": r.get("url"),
                # 截断到300字符控制喂回模型的token占用，避免一次搜索把上下文撑爆
                "content": (r.get("content") or "")[:300],
            }
            for r in (data.get("results") or [])
        ]
        return json.dumps(
            {"answer": data.get("answer"), "results": results}, ensure_ascii=False
        )
    except requests.exceptions.Timeout:
        return json.dumps({"error": "搜索超时，请稍后再试。"}, ensure_ascii=False)
    except requests.exceptions.HTTPError as e:
        status = e.response.status_code if e.response is not None else "?"
        if status == 401:
            return json.dumps({"error": "Tavily API Key 无效，请在设置中检查。"}, ensure_ascii=False)
        if status == 429:
            return json.dumps({"error": "搜索请求过于频繁，请稍后再试。"}, ensure_ascii=False)
        return json.dumps({"error": f"搜索请求失败 (HTTP {status})"}, ensure_ascii=False)
    except requests.exceptions.RequestException as e:
        return json.dumps({"error": f"搜索请求失败: {e}"}, ensure_ascii=False)
