"""
图像/视频生成异步工作线程 (Agnes AI)。

图像生成一次性返回，视频生成是"提交任务 -> 轮询结果"的异步模式，
封装成独立线程避免阻塞 UI，同时支持取消。
"""

import base64
import time

import requests
from PyQt6.QtCore import QThread, pyqtSignal

from src.providers.base import ProviderError

# 轮询默认参数(VideoGenWorker 与 video_pipeline 共用)
POLL_INTERVAL = 5  # 秒(基础轮询间隔)
BACKOFF_INTERVAL = 20  # 秒(遇限流/瞬时错误的退避间隔)
MAX_WAIT = 900  # 最长等待15分钟
MAX_CONSECUTIVE_ERRORS = 6  # 连续瞬时错误上限


def poll_video_result(
    provider,
    video_id: str,
    on_progress=None,
    should_cancel=None,
    poll_interval: int = POLL_INTERVAL,
    backoff_interval: int = BACKOFF_INTERVAL,
    max_wait: int = MAX_WAIT,
    max_consecutive_errors: int = MAX_CONSECUTIVE_ERRORS,
) -> str:
    """阻塞轮询 Agnes 视频任务，返回 video_url。

    - on_progress(str): 可选进度回调
    - should_cancel() -> bool: 可选取消检查回调，返回 True 时中止并返回 None
    - 成功返回 video_url 字符串；被取消返回 None；失败/超时抛 ProviderError

    完成判断只看 video_url 是否非空(不依赖 status 精确匹配"completed"，
    因为实测服务端完成时字段是 url 不是 video_url，且 status 取值不稳定)。
    """
    from src.logger import get_logger

    logger = get_logger()
    waited = 0
    consecutive_errors = 0

    while waited < max_wait:
        if should_cancel is not None and should_cancel():
            return None

        interval = poll_interval
        try:
            result = provider.get_video_result(video_id)
            consecutive_errors = 0
            status = result.get("status")
            video_url = result.get("video_url")
            logger.info(f"视频任务轮询({video_id}): {result.get('raw')}")

            if video_url:
                return video_url
            if status in ("failed", "error"):
                detail = result.get("error")
                raise ProviderError(
                    f"视频生成失败(状态: {status})" + (f"：{detail}" if detail else "。")
                )
            progress = result.get("progress")
            if on_progress is not None:
                on_progress(f"生成中... {progress if progress is not None else status}")
        except ProviderError as e:
            msg = str(e)
            # failed/error 状态、以及内容审核拒绝(content_policy_violation)都是终态，
            # 重试没有意义(审核拒绝不会因为多等一会儿就通过)，直接上抛；
            # 其余(限流/网络波动)才退避重试。
            if "视频生成失败" in msg or "content_policy_violation" in msg:
                raise
            consecutive_errors += 1
            if consecutive_errors > max_consecutive_errors:
                raise ProviderError(f"多次重试仍失败：{e}")
            interval = backoff_interval
            if on_progress is not None:
                on_progress(
                    f"遇到限流/网络波动，{interval}s 后自动重试"
                    f"({consecutive_errors}/{max_consecutive_errors})... {e}"
                )

        time.sleep(interval)
        waited += interval

    raise ProviderError("视频生成超时，请稍后在 Agnes AI 控制台查看任务结果。")


class ImageGenWorker(QThread):
    """图像生成工作线程。完成后直接把图像原始字节回传给 UI 层。"""

    image_ready = pyqtSignal(bytes)
    error_occurred = pyqtSignal(str)

    def __init__(self, provider, prompt: str, size: str, image_path: str = None):
        super().__init__()
        self.provider = provider
        self.prompt = prompt
        self.size = size
        self.image_path = image_path

    def run(self):
        try:
            result = self.provider.generate_image(self.prompt, self.size, self.image_path)
            image_bytes = self._to_bytes(result)
            if not image_bytes:
                self.error_occurred.emit("图像生成失败：响应中未找到图像数据。")
                return
            self.image_ready.emit(image_bytes)
        except ProviderError as e:
            self.error_occurred.emit(str(e))
        except Exception as e:
            self.error_occurred.emit(f"出错了: {e}")

    @staticmethod
    def _to_bytes(result: dict):
        if result.get("b64_json"):
            try:
                return base64.b64decode(result["b64_json"])
            except Exception:
                return None
        if result.get("url"):
            try:
                resp = requests.get(result["url"], timeout=60)
                resp.raise_for_status()
                return resp.content
            except Exception:
                return None
        return None


class VideoGenWorker(QThread):
    """视频生成工作线程：提交任务后轮询直到完成/失败/取消/超时。

    轮询阶段对限流(429)等瞬时错误做退避重试，而不是一遇到就整体失败，
    避免"生成到一半被限流直接报错"的糟糕体验。
    """

    progress_update = pyqtSignal(str)  # 状态文案
    video_ready = pyqtSignal(str)  # video_url
    error_occurred = pyqtSignal(str)

    POLL_INTERVAL = 5  # 秒(基础轮询间隔，放宽一点降低触发限流的概率)
    BACKOFF_INTERVAL = 20  # 秒(遇到限流/瞬时错误后的退避间隔)
    MAX_WAIT = 900  # 最长等待15分钟
    MAX_CONSECUTIVE_ERRORS = 6  # 连续瞬时错误上限，超过才判定失败

    def __init__(self, provider, prompt: str, width: int = 1152, height: int = 768):
        super().__init__()
        self.provider = provider
        self.prompt = prompt
        self.width = width
        self.height = height
        self._cancel = False

    def cancel(self):
        self._cancel = True

    def run(self):
        # 1) 提交任务(失败无法继续，直接报错)
        try:
            self.progress_update.emit("正在提交视频生成任务...")
            video_id = self.provider.create_video_task(
                self.prompt, width=self.width, height=self.height
            )
        except ProviderError as e:
            self.error_occurred.emit(str(e))
            return
        except Exception as e:
            self.error_occurred.emit(f"出错了: {e}")
            return

        # 2) 轮询结果(复用共享轮询函数)
        try:
            video_url = poll_video_result(
                self.provider,
                video_id,
                on_progress=self.progress_update.emit,
                should_cancel=lambda: self._cancel,
            )
        except ProviderError as e:
            self.error_occurred.emit(str(e))
            return
        if video_url is None:
            self.progress_update.emit("已取消。")
            return
        self.video_ready.emit(video_url)
