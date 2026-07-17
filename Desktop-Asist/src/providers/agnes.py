"""
Agnes AI Provider - OpenAI 兼容的文本/多模态对话，另外提供图像生成与视频生成能力。

图像/视频生成不是标准 /chat/completions 接口，因此作为额外方法暴露在
AgnesProvider 上，不属于 BaseProvider 通用接口(DeepSeek 等纯文本供应商没有)。

参考: https://agnes-ai.com/doc ,
      https://agnes-ai.com/doc/agnes-image-21-flash ,
      https://agnes-ai.com/doc/agnes-video-v20
"""

import base64
import json
import mimetypes

import requests

from src.providers.openai_compatible import OpenAICompatibleProvider
from src.providers.base import ProviderError


class AgnesProvider(OpenAICompatibleProvider):
    """Agnes AI API 客户端。"""

    id = "agnes"
    display_name = "Agnes AI"
    MODEL_CHOICES = [
        ("agnes-2.0-flash", "Agnes 2.0 Flash — 文本/多模态"),
    ]
    IMAGE_MODEL = "agnes-image-2.1-flash"
    VIDEO_MODEL = "agnes-video-v2.0"

    SYSTEM_PROMPT = (
        "你是 AI 小精灵，一个实用的桌面 AI 助手。"
        "你用中文回答问题，简洁明了，友好有帮助。"
        "如需查询实时信息(天气/当前时间/系统资源状态/本地文件/日历日程)或做精确数学计算，"
        "请主动调用相应工具获取真实数据，不要凭猜测或声称无法访问网络来回避。"
    )

    # ===== 图像生成(文生图/图生图) =====

    def generate_image(
        self, prompt: str, size: str = "1024x1024", image_path: str = None,
        prefer_url: bool = False, timeout: int = 60
    ) -> dict:
        """调用 /images/generations。返回 {"url": ..., "b64_json": ...}。

        prefer_url=True 时不请求 base64，直接拿 Agnes CDN 的公开 url(用作
        视频生成 image 或 extra_body.image 的参考图，必须是公开可访问URL)。
        """
        url = f"{self.base_url}/images/generations"
        payload = {"model": self.IMAGE_MODEL, "prompt": prompt, "size": size}
        if image_path:
            # 图生图: Agnes 要求把输入图放到 extra_body.image 中。
            payload["extra_body"] = {
                "image": [self._file_to_data_uri(image_path)],
                "response_format": "url" if prefer_url else "b64_json",
            }
        elif prefer_url:
            payload["extra_body"] = {"response_format": "url"}
        elif not prefer_url:
            # 文生图默认: 顶层 return_base64=true 直接拿 base64(不要把 response_format
            # 放在顶层, 官方文档特别强调这一点)。prefer_url 时不设这个，走默认返回 url。
            payload["return_base64"] = True

        try:
            resp = requests.post(url, json=payload, headers=self._headers(), timeout=timeout)
            resp.raise_for_status()
            data = resp.json()
            items = data.get("data") or []
            item = items[0] if items else {}
            return {
                "url": item.get("url"),
                "b64_json": item.get("b64_json") or data.get("image_base64"),
            }
        except requests.exceptions.Timeout:
            raise ProviderError("图像生成超时，请稍后再试。")
        except requests.exceptions.ConnectionError:
            raise ProviderError("网络连接失败，请检查网络。")
        except requests.exceptions.HTTPError as e:
            raise ProviderError(self._describe_http_error(e))
        except (KeyError, IndexError, json.JSONDecodeError):
            raise ProviderError("图像服务响应异常，请稍后再试。")

    # ===== 视频生成(异步任务: 提交 -> 轮询) =====

    def create_video_task(
        self,
        prompt: str,
        width: int = 1152,
        height: int = 768,
        num_frames: int = 121,
        frame_rate: int = 24,
        image_url: str = None,
        image_urls: list = None,
        mode: str = None,
        seed: int = None,
        negative_prompt: str = None,
        timeout: int = 30,
    ) -> str:
        """提交文生视频任务，返回 video_id(用于后续轮询结果)。

        image_url: 单张首帧图 URL，放进顶层 image，用于 image-to-video。
        image_urls: 多关键帧 URL 列表，放进 extra_body.image，用于 keyframes 模式。
        mode: 单图模式放进顶层 mode；多关键帧模式放进 extra_body.mode。
        不传 image_url/image_urls 就是纯文生视频。
        """
        url = f"{self.base_url}/videos"
        payload = {
            "model": self.VIDEO_MODEL,
            "prompt": prompt,
            "width": width,
            "height": height,
            "num_frames": num_frames,
            "frame_rate": frame_rate,
        }
        if image_url:
            payload["image"] = image_url
        if mode and not image_urls:
            payload["mode"] = mode
        if seed is not None:
            payload["seed"] = seed
        if negative_prompt:
            payload["negative_prompt"] = negative_prompt
        extra_body = {}
        if image_urls:
            extra_body["image"] = image_urls
        if mode and image_urls:
            extra_body["mode"] = mode
        if extra_body:
            payload["extra_body"] = extra_body
        try:
            resp = requests.post(url, json=payload, headers=self._headers(), timeout=timeout)
            resp.raise_for_status()
            data = resp.json()
            video_id = data.get("video_id") or data.get("task_id")
            if not video_id:
                raise ProviderError("视频任务创建失败：响应中缺少 video_id。")
            return video_id
        except requests.exceptions.Timeout:
            raise ProviderError("视频任务提交超时，请稍后再试。")
        except requests.exceptions.ConnectionError:
            raise ProviderError("网络连接失败，请检查网络。")
        except requests.exceptions.HTTPError as e:
            raise ProviderError(self._describe_http_error(e))
        except (KeyError, json.JSONDecodeError):
            raise ProviderError("视频任务响应异常，请稍后再试。")
        except OSError as e:
            raise ProviderError(f"本地网络环境异常: {e}")

    def get_video_result(self, video_id: str, timeout: int = 30) -> dict:
        """按 video_id 轮询视频任务结果。

        返回 {"status": ..., "progress": ..., "video_url": ..., "error": ..., "raw": 原始响应}。

        踩过的坑记录: 视频真正完成时，服务端顶层字段是 `url`，不是文档字面
        描述的 `video_url`(实测返回样例: {"status": "completed", "progress":
        100, "url": "https://platform-outputs.agnes-ai.space/videos/...",
        "error": None, ...})。之前代码读 data.get("video_url") 永远是 None，
        导致哪怕任务早就完成、进度显示100%，也一直判断不了"已完成"，
        直到轮询超时。这里同时兼容 url/video_url 两种字段名，双保险。
        """
        # /agnesapi 查询端点不带 /v1 前缀，与 chat/images/videos 端点不同
        base = self.base_url[:-3] if self.base_url.endswith("/v1") else self.base_url
        url = f"{base}/agnesapi"
        try:
            resp = requests.get(
                url, params={"video_id": video_id}, headers=self._headers(), timeout=timeout
            )
            resp.raise_for_status()
            data = resp.json()
            return {
                "status": data.get("status", "unknown"),
                "progress": data.get("progress"),
                "video_url": data.get("url") or data.get("video_url"),
                "error": data.get("error"),
                "raw": data,
            }
        except requests.exceptions.Timeout:
            raise ProviderError("查询视频结果超时，请稍后再试。")
        except requests.exceptions.ConnectionError:
            raise ProviderError("网络连接失败，请检查网络。")
        except requests.exceptions.HTTPError as e:
            raise ProviderError(self._describe_http_error(e))
        except json.JSONDecodeError:
            raise ProviderError("视频结果响应异常，请稍后再试。")
        except OSError as e:
            # 证书路径缺失等环境问题("Could not find a suitable TLS CA
            # certificate bundle"是 OSError，不是 requests.exceptions 下的
            # 网络异常，不会被上面几个分支捕获)，转成可读提示并纳入轮询退避重试，
            # 不让原始技术错误信息直接抛给用户、也不让整个视频生成任务放弃重试。
            raise ProviderError(f"本地网络环境异常: {e}")

    # ===== 多模态(视觉理解)辅助方法 =====

    @staticmethod
    def build_vision_content(text: str, image_paths: list) -> list:
        """构建 OpenAI 风格的多模态消息 content 数组(文本 + 本地图片)。

        本地图片直接编码成 Data URI Base64 内联传输，不依赖额外的文件上传接口。
        """
        content = [{"type": "text", "text": text}]
        for path in image_paths or []:
            content.append(
                {
                    "type": "image_url",
                    "image_url": {"url": AgnesProvider._file_to_data_uri(path)},
                }
            )
        return content

    @staticmethod
    def _file_to_data_uri(path: str) -> str:
        mime, _ = mimetypes.guess_type(path)
        mime = mime or "image/png"
        with open(path, "rb") as f:
            b64 = base64.b64encode(f.read()).decode("ascii")
        return f"data:{mime};base64,{b64}"
