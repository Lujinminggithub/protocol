"""
异步聊天请求线程 - provider-agnostic，支持流式与非流式两种模式，
以及标准 Function Calling(工具调用)的完整"调用→执行→喂回"循环。
"""

import json
import threading

from PyQt6.QtCore import QThread, pyqtSignal

from src.providers.base import ProviderError
from src.tools import registry as tool_registry

MAX_TOOL_ROUNDS = 5  # 单次用户请求内最多允许的"工具调用→喂回结果→再生成"往返轮数


class ChatWorker(QThread):
    """异步聊天请求线程。"""

    # 信号: 收到一段思考过程增量文本
    reasoning_delta = pyqtSignal(str)
    # 信号: 收到一段最终答案增量文本
    content_delta = pyqtSignal(str)
    # 信号: 开始执行一个工具调用 (call_id, tool_name, args_display_json)
    tool_call_started = pyqtSignal(str, str, str)
    # 信号: 一个工具调用执行完成 (call_id, tool_name, args_display_json, result_display)
    tool_call_finished = pyqtSignal(str, str, str, str)
    # 信号: 完成，(content, reasoning) —— 避免与 QThread 内建的 finished 信号同名冲突
    finished_ok = pyqtSignal(str, str)
    # 信号: 出错
    error_occurred = pyqtSignal(str)

    def __init__(self, provider, messages: list, stream: bool = True, tools: list = None):
        super().__init__()
        self.provider = provider
        self.messages = messages
        self.stream = stream
        self.tools = tools or []
        self._cancel_event = threading.Event()

    def cancel(self):
        """请求取消生成(流式模式下会主动断开连接)。"""
        self._cancel_event.set()

    def run(self):
        try:
            # 本次请求专属的工作副本：工具调用往返产生的 assistant(tool_calls)/tool
            # 消息只存在于这里，不回写到调用方持有的 self.messages（那是跨轮次持久化
            # 历史的引用，只应该追加最终的用户可见回复）。
            working_messages = list(self.messages)

            for round_idx in range(MAX_TOOL_ROUNDS + 1):
                if self._cancel_event.is_set():
                    return

                if self.stream:
                    result = self.provider.chat_stream(
                        working_messages,
                        on_reasoning_delta=self.reasoning_delta.emit,
                        on_content_delta=self.content_delta.emit,
                        cancel_event=self._cancel_event,
                        tools=self.tools,
                    )
                else:
                    result = self.provider.chat(working_messages, tools=self.tools)

                if not result.tool_calls:
                    self.finished_ok.emit(result.content, result.reasoning)
                    return

                if round_idx == MAX_TOOL_ROUNDS:
                    fallback = result.content or "(工具调用次数超出上限，已停止继续调用)"
                    self.finished_ok.emit(fallback, result.reasoning)
                    return

                # 1) assistant(带tool_calls)消息追加进本次请求的临时上下文
                assistant_msg = {
                    "role": "assistant",
                    "content": result.content or None,
                    "tool_calls": [
                        {
                            "id": tc["id"],
                            "type": "function",
                            "function": {
                                "name": tc["name"],
                                "arguments": tc["arguments_raw"],
                            },
                        }
                        for tc in result.tool_calls
                    ],
                }
                # DeepSeek Thinking 模式例外规则: 只有这种"assistant消息带tool_calls"
                # 的情况才需要保留 reasoning_content 一起传回(否则下一轮请求 400)。
                # 这个例外只影响 working_messages 这个本次请求的临时列表，
                # 不会污染跨对话轮次持久化的 chat_history(那里始终只存最终 content)。
                if result.reasoning:
                    assistant_msg["reasoning_content"] = result.reasoning
                working_messages.append(assistant_msg)

                # 2) 逐个执行工具调用(支持并行多调用)，都要产出对应 role=tool 消息
                for tc in result.tool_calls:
                    if self._cancel_event.is_set():
                        return
                    args_display = json.dumps(tc["arguments"], ensure_ascii=False)
                    self.tool_call_started.emit(tc["id"], tc["name"], args_display)
                    result_str = tool_registry.execute_tool(tc["name"], tc["arguments"])
                    self.tool_call_finished.emit(tc["id"], tc["name"], args_display, result_str)
                    working_messages.append(
                        {"role": "tool", "tool_call_id": tc["id"], "content": result_str}
                    )
                # 3) 回到循环顶部，把工具结果喂回模型继续生成
        except ProviderError as e:
            self.error_occurred.emit(str(e))
        except Exception as e:
            self.error_occurred.emit(f"出错了: {e}")
