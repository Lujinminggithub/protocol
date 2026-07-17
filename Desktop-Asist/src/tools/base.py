"""
工具规格定义 - Function Calling 里每个可调用工具的元数据与执行函数。
"""

from dataclasses import dataclass
from typing import Callable, Optional


@dataclass
class ToolSpec:
    """一个可供 AI 调用的工具。"""

    name: str
    description: str
    parameters: dict  # JSON Schema，直接放进 function.parameters
    func: Callable[[dict], str]  # 输入已解析的 arguments dict，输出给模型看的字符串(建议JSON字符串)
    # 若非 None，表示该工具需要用户配置对应的 Key(如 "tavily")才会出现在模型可见的工具列表里
    requires: Optional[str] = None

    def to_openai_schema(self) -> dict:
        """转成 OpenAI 兼容的 tools[] 里单个工具的 JSON 结构。"""
        return {
            "type": "function",
            "function": {
                "name": self.name,
                "description": self.description,
                "parameters": self.parameters,
            },
        }
