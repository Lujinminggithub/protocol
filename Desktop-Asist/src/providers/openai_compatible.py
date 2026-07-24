"""
OpenAI 兼容 Provider 基类 - 封装标准 /chat/completions 的流式/非流式调用逻辑，
供 DeepSeek、Agnes AI 等兼容 OpenAI 接口格式的供应商复用。
"""

import json

import requests

from src.providers.base import BaseProvider, ChatResult, ProviderError
from src.video_pipeline import prompt_audit


class OpenAICompatibleProvider(BaseProvider):
    """实现标准 OpenAI /chat/completions 流式/非流式调用的通用逻辑。

    子类可覆盖 `_build_payload` 增加供应商专属字段(如 DeepSeek 的 thinking 模式)。
    """

    def _build_payload(self, messages: list, stream: bool, tools: list = None) -> dict:
        payload = {
            "model": self.model,
            "messages": messages,
            "max_tokens": self.max_tokens,
            "temperature": self.temperature,
            "stream": stream,
        }
        if tools:
            payload["tools"] = tools
        return payload

    def _headers(self) -> dict:
        return {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }

    def chat(self, messages: list, timeout: int = 60, tools: list = None) -> ChatResult:
        """非流式请求，直接返回完整结果。"""
        url = f"{self.base_url}/chat/completions"
        payload = self._build_payload(messages, stream=False, tools=tools)
        prompt_audit.record("chat_prompt", payload, label=self.model)
        try:
            resp = requests.post(
                url,
                json=payload,
                headers=self._headers(),
                timeout=timeout,
            )
            resp.raise_for_status()
            msg = resp.json()["choices"][0]["message"]
            return ChatResult(
                content=msg.get("content", "") or "",
                reasoning=msg.get("reasoning_content", "") or "",
                tool_calls=self._parse_tool_calls(msg.get("tool_calls")),
            )
        except requests.exceptions.Timeout:
            raise ProviderError("请求超时，请稍后再试。")
        except requests.exceptions.ConnectionError:
            raise ProviderError("网络连接失败，请检查网络。")
        except requests.exceptions.HTTPError as e:
            raise ProviderError(self._describe_http_error(e))
        except (KeyError, IndexError, json.JSONDecodeError):
            raise ProviderError("服务响应异常，请稍后再试。")
        except OSError as e:
            # 例如打包环境下证书路径缺失("Could not find a suitable TLS CA
            # certificate bundle")，不属于 requests.exceptions 下的网络异常，
            # 转成可读提示而不是把原始技术错误信息抛给用户。
            raise ProviderError(f"本地网络环境异常: {e}")

    def chat_stream(
        self,
        messages: list,
        on_reasoning_delta,
        on_content_delta,
        timeout: int = 120,
        cancel_event=None,
        tools: list = None,
    ) -> ChatResult:
        """流式请求。逐行解析 SSE，分离思考过程/最终答案/工具调用增量。"""
        url = f"{self.base_url}/chat/completions"
        headers = {**self._headers(), "Accept": "text/event-stream"}
        payload = self._build_payload(messages, stream=True, tools=tools)

        reasoning_parts = []
        content_parts = []
        finish_reason = None
        # 按 tool_call 的 index 分组累积片段(并行工具调用时 index 用来区分是哪一个)
        tool_call_acc = {}

        try:
            with requests.post(
                url, json=payload, headers=headers, timeout=timeout, stream=True
            ) as resp:
                resp.raise_for_status()
                # 关键: 用字节迭代再手动按 UTF-8 解码。
                # 若用 iter_lines(decode_unicode=True)，当 SSE 响应头未声明 charset 时
                # requests 会默认用 latin-1 解码，导致中文变乱码。
                for raw_line in resp.iter_lines():
                    if cancel_event is not None and cancel_event.is_set():
                        resp.close()
                        break
                    if not raw_line:
                        continue  # SSE 心跳空行
                    line = raw_line.decode("utf-8", errors="replace")
                    if not line.startswith("data:"):
                        continue
                    data_str = line[len("data:"):].strip()
                    if data_str == "[DONE]":
                        break
                    try:
                        chunk = json.loads(data_str)
                    except json.JSONDecodeError:
                        continue  # 个别脏行不应中断整个流
                    choices = chunk.get("choices") or []
                    if not choices:
                        continue
                    delta = choices[0].get("delta", {}) or {}
                    finish_reason = choices[0].get("finish_reason") or finish_reason

                    r = delta.get("reasoning_content")
                    if r:
                        reasoning_parts.append(r)
                        on_reasoning_delta(r)

                    c = delta.get("content")
                    if c:
                        content_parts.append(c)
                        on_content_delta(c)

                    tc_deltas = delta.get("tool_calls")
                    if tc_deltas:
                        for tc in tc_deltas:
                            idx = tc.get("index", 0)
                            slot = tool_call_acc.setdefault(
                                idx, {"id": "", "name": "", "arguments": ""}
                            )
                            if tc.get("id"):
                                slot["id"] += tc["id"]
                            func = tc.get("function") or {}
                            if func.get("name"):
                                slot["name"] += func["name"]
                            if func.get("arguments"):
                                slot["arguments"] += func["arguments"]
        except requests.exceptions.Timeout:
            raise ProviderError("请求超时，请稍后再试。")
        except requests.exceptions.ConnectionError:
            raise ProviderError("网络连接失败，请检查网络。")
        except requests.exceptions.HTTPError as e:
            raise ProviderError(self._describe_http_error(e))
        except OSError as e:
            raise ProviderError(f"本地网络环境异常: {e}")

        return ChatResult(
            content="".join(content_parts),
            reasoning="".join(reasoning_parts),
            finish_reason=finish_reason,
            tool_calls=self._finalize_tool_call_acc(tool_call_acc),
        )

    @staticmethod
    def _parse_tool_calls(raw_tool_calls) -> list:
        """解析非流式响应里的 message.tool_calls。"""
        if not raw_tool_calls:
            return []
        result = []
        for i, tc in enumerate(raw_tool_calls):
            func = tc.get("function") or {}
            raw_args = func.get("arguments") or "{}"
            try:
                args = json.loads(raw_args)
                if not isinstance(args, dict):
                    args = {}
            except json.JSONDecodeError:
                args = {}
            result.append(
                {
                    "id": tc.get("id") or f"call_{i}",
                    "name": func.get("name", ""),
                    "arguments": args,
                    "arguments_raw": raw_args,
                }
            )
        return result

    @staticmethod
    def _finalize_tool_call_acc(tool_call_acc: dict) -> list:
        """把流式累积的 tool_call 片段解析成最终的 tool_calls 列表。"""
        result = []
        for idx in sorted(tool_call_acc):
            slot = tool_call_acc[idx]
            raw = slot["arguments"] or "{}"
            try:
                args = json.loads(raw)
                if not isinstance(args, dict):
                    args = {}
            except json.JSONDecodeError:
                # 模型可能生成非法JSON(截断/幻觉)，兜底成空dict，交给工具执行层再校验必填参数
                args = {}
            result.append(
                {
                    "id": slot["id"] or f"call_{idx}",
                    "name": slot["name"],
                    "arguments": args,
                    "arguments_raw": raw,
                }
            )
        return result

    def _describe_http_error(self, e: requests.exceptions.HTTPError) -> str:
        status = e.response.status_code if e.response is not None else "?"
        body = ""
        try:
            body = e.response.text[:200]
        except Exception:
            pass
        if status == 429:
            return "请求过于频繁(HTTP 429)，触发了服务端限流，请稍等一会儿再试。"
        if status == 401:
            return "API Key 无效或已过期(HTTP 401)，请在设置中检查 Token。"
        return f"API 请求失败 (HTTP {status})：{body}"
