"""
Provider 抽象基类 - 为后续接入其他AI供应商预留接口，当前只实现 DeepSeek。
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Callable, Optional


class ProviderError(Exception):
    """AI 供应商调用出错(网络/超时/HTTP错误等)，消息为面向用户的可读文案。"""

    pass


@dataclass
class ChatResult:
    """一次对话请求的结果。"""

    content: str = ""
    reasoning: str = ""
    finish_reason: Optional[str] = None
    # 每项: {"id": str, "name": str, "arguments": dict, "arguments_raw": str}
    # arguments 是已 json.loads 解析好的 dict(供工具执行直接用)；
    # arguments_raw 是模型原样吐出的 JSON 字符串(回填 assistant 消息时用这个，
    # 不要用 json.dumps(arguments) 重新序列化，避免数字精度/字段顺序细微差异)
    tool_calls: list = field(default_factory=list)


class BaseProvider(ABC):
    """AI 供应商客户端基类。"""

    id: str = "base"
    display_name: str = "Base Provider"
    MODEL_CHOICES: list = []

    def __init__(
        self,
        base_url: str,
        api_key: str,
        model: str,
        temperature: float = 0.7,
        max_tokens: int = 2048,
        **kwargs,
    ):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.model = model
        self.temperature = temperature
        self.max_tokens = max_tokens

    @abstractmethod
    def chat(self, messages: list, timeout: int = 60, tools: list = None) -> ChatResult:
        """非流式请求，直接返回完整结果。tools 为 OpenAI 风格的工具schema列表(可选)。"""
        ...

    @abstractmethod
    def chat_stream(
        self,
        messages: list,
        on_reasoning_delta: Callable[[str], None],
        on_content_delta: Callable[[str], None],
        timeout: int = 120,
        cancel_event=None,
        tools: list = None,
    ) -> ChatResult:
        """流式请求，增量回调思考过程与最终答案，返回完整结果(含累积解析好的tool_calls)。"""
        ...
