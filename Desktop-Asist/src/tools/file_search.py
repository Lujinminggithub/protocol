"""
本地文件搜索工具 - 限制在用户主目录内，供 AI 按需查找文件。

安全边界: 只允许搜索用户主目录(~)及其子目录，拒绝越界路径(如 C:\\Windows
或用 .. 逃逸)，并显式排除本应用自己的配置目录(~/.aisprite，避免被诱导
枚举出加密的 API Key 存放位置等敏感路径)。只返回文件路径列表，不读取
文件内容，进一步降低敏感信息泄露面。
"""

import fnmatch
import json
import os
import time

from src.paths import USER_DATA_DIR

_MAX_RESULTS = 200
_MAX_SCAN_SECONDS = 5
_MAX_SCAN_FILES = 20000


def _build_pattern(query: str) -> str:
    """无通配符时自动包裹为 *query* 做子串匹配(与 search_panel.py 体验一致)。"""
    if any(ch in query for ch in "*?["):
        return query
    return f"*{query}*"


def _resolve_safe_directory(directory: str):
    """校验并返回安全的搜索根目录；越界返回 None。"""
    home = os.path.realpath(os.path.expanduser("~"))
    target = os.path.realpath(directory) if directory else home
    try:
        common = os.path.commonpath([target, home])
    except ValueError:
        # 不同盘符(Windows)，commonpath 会抛异常，视为越界
        return None
    if os.path.normcase(common) != os.path.normcase(home):
        return None
    return target


def run(args: dict) -> str:
    pattern_input = str(args.get("pattern", "")).strip()
    if not pattern_input:
        return json.dumps({"error": "pattern 不能为空"}, ensure_ascii=False)

    directory = str(args.get("directory", "") or "").strip()
    safe_dir = _resolve_safe_directory(directory)
    if safe_dir is None:
        return json.dumps(
            {"error": "目录必须在用户主目录内，不允许访问系统目录或越界路径。"},
            ensure_ascii=False,
        )
    if not os.path.isdir(safe_dir):
        return json.dumps({"error": f"目录不存在: {safe_dir}"}, ensure_ascii=False)

    excluded = os.path.normcase(os.path.realpath(USER_DATA_DIR))
    pattern = _build_pattern(pattern_input).lower()

    results = []
    scanned = 0
    start = time.monotonic()
    truncated = False

    for root, dirs, files in os.walk(safe_dir):
        if os.path.normcase(os.path.realpath(root)).startswith(excluded):
            dirs[:] = []  # 不进入应用自身配置目录
            continue
        if time.monotonic() - start > _MAX_SCAN_SECONDS:
            truncated = True
            break
        for filename in files:
            scanned += 1
            if scanned > _MAX_SCAN_FILES:
                truncated = True
                break
            if fnmatch.fnmatch(filename.lower(), pattern):
                results.append(os.path.join(root, filename))
                if len(results) >= _MAX_RESULTS:
                    truncated = True
                    break
        if truncated:
            break

    return json.dumps(
        {"count": len(results), "truncated": truncated, "files": results},
        ensure_ascii=False,
    )
