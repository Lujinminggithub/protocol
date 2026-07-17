"""
DeepSeek Provider - 对接 DeepSeek V4 (deepseek-v4-flash / deepseek-v4-pro)

支持流式/非流式调用，以及 Thinking Mode(深度思考)下的 reasoning_content
(思维链) 与 content(最终答案) 分离返回。

参考:
- https://api-docs.deepseek.com/guides/reasoning_model
- https://api-docs.deepseek.com/guides/thinking_mode
- https://api-docs.deepseek.com/guides/reasoning_model_api_example_streaming

注意: deepseek-chat / deepseek-reasoner 两个旧模型名已于 2026-07-24 停用，
本客户端只面向新模型 deepseek-v4-flash / deepseek-v4-pro。
"""

from src.providers.openai_compatible import OpenAICompatibleProvider


class DeepSeekProvider(OpenAICompatibleProvider):
    """DeepSeek API 客户端。"""

    id = "deepseek"
    display_name = "DeepSeek"
    MODEL_CHOICES = [
        ("deepseek-v4-flash", "DeepSeek V4 Flash — 高性价比"),
        ("deepseek-v4-pro", "DeepSeek V4 Pro — 旗舰"),
    ]

    SYSTEM_PROMPT = (
        "你是 AI 小精灵，一个实用的桌面 AI 助手。"
        "你用中文回答问题，简洁明了，友好有帮助。"
        "如果用户问非知识类问题，尽量给出有用的回答。"
        "如需查询实时信息(天气/当前时间/系统资源状态/本地文件/日历日程)或做精确数学计算，"
        "请主动调用相应工具获取真实数据，不要凭猜测或声称无法访问网络来回避。"
    )

    def __init__(
        self,
        *args,
        thinking_enabled: bool = True,
        reasoning_effort: str = "high",
        **kwargs,
    ):
        super().__init__(*args, **kwargs)
        self.thinking_enabled = thinking_enabled
        # 仅 "high" | "max" 有意义: DeepSeek 服务端会把 low/medium 映射为 high,
        # xhigh 映射为 max, 因此上层 UI 也只暴露这两档。
        self.reasoning_effort = reasoning_effort

    def _build_payload(self, messages: list, stream: bool, tools: list = None) -> dict:
        payload = {
            "model": self.model,
            # messages 由调用方(ChatWorker)构造: 正常轮次只含 role/content，
            # 绝不带 reasoning_content；唯一例外是"assistant消息带tool_calls"时，
            # DeepSeek 要求该条消息必须保留 reasoning_content 一起传回(否则400)，
            # 这个例外由 ChatWorker 在拼接消息时处理，这里只负责原样透传。
            "messages": messages,
            "max_tokens": self.max_tokens,
            "stream": stream,
        }
        if tools:
            payload["tools"] = tools
        if self.thinking_enabled:
            # thinking / reasoning_effort 是裸 HTTP 请求的顶层字段
            # (OpenAI SDK 里通过 extra_body 传, 但我们是直接发 JSON, 无需该包装层)
            payload["thinking"] = {"type": "enabled"}
            payload["reasoning_effort"] = self.reasoning_effort
            # thinking 模式下 temperature/top_p 等采样参数不生效, 不发送
        else:
            payload["thinking"] = {"type": "disabled"}
            payload["temperature"] = self.temperature
        return payload

    def _describe_http_error(self, e) -> str:
        status = e.response.status_code if e.response is not None else "?"
        body = ""
        try:
            body = e.response.text[:200]
        except Exception:
            pass
        if status in (400, 404) and "model" in body.lower():
            return (
                "模型不可用（可能已下线）。DeepSeek 已于 2026-07-24 停用 "
                "deepseek-chat / deepseek-reasoner，请在设置中选择 "
                "deepseek-v4-flash 或 deepseek-v4-pro。"
            )
        # 其余情况(429限流/401鉴权等)交给基类统一给出友好提示
        return super()._describe_http_error(e)
