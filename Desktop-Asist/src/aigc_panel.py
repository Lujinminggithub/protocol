"""
AIGC面板 - AI生成内容工具

文本类任务(文本生成/代码生成/图片描述)使用当前配置的对话 Provider(DeepSeek/Agnes AI)；
图像生成/视频生成/多模态理解固定使用 Agnes AI(DeepSeek 不提供这些能力)。
"""

import os
from datetime import datetime

from PyQt6.QtWidgets import (
    QVBoxLayout,
    QHBoxLayout,
    QWidget,
    QLabel,
    QPushButton,
    QTextEdit,
    QComboBox,
    QGroupBox,
    QLineEdit,
    QFileDialog,
    QStackedWidget,
    QScrollArea,
    QApplication,
)
from PyQt6.QtCore import Qt, QTimer, QUrl
from PyQt6.QtGui import QFont, QPixmap, QDesktopServices

from src.config import get_config
from src.providers import registry
from src.providers.agnes import AgnesProvider
from src.chat_worker import ChatWorker
from src.media_worker import ImageGenWorker, VideoGenWorker


# 任务类型定义: kind 决定走哪种生成流程
# - "chat": 走 ChatWorker，使用当前配置的对话 Provider(DeepSeek/Agnes AI)
# - "image": 图像生成，固定 Agnes AI
# - "video": 视频生成(异步任务)，固定 Agnes AI
# - "vision": 多模态图文理解，固定 Agnes AI
TASK_TYPES = [
    {
        "key": "text",
        "label": "文本生成",
        "kind": "chat",
        "placeholder": "请输入写作主题或要求...",
        "system_prompt": (
            "你是一个优秀的文案创作助手。请根据用户的提示，生成高质量的文章或文本。"
            "要求：语言流畅、结构清晰、内容丰富。"
        ),
    },
    {
        "key": "code",
        "label": "代码生成",
        "kind": "chat",
        "placeholder": "请描述你需要什么功能的代码...",
        "system_prompt": (
            "你是一个专业的程序员。请根据用户的需求生成代码。"
            "要求：代码规范、有注释、考虑边界情况。"
            "使用 Markdown 格式包裹代码块，标注语言类型。"
        ),
    },
    {
        "key": "image_desc",
        "label": "图片描述(文本)",
        "kind": "chat",
        "placeholder": "请描述你想生成的画面内容...",
        "system_prompt": (
            "你是一个图像描述专家。请根据用户的描述提示，生成一段生动详细的画面描述文字。"
            "要求：包含色彩、构图、光影、氛围等细节描写。"
        ),
    },
    {
        "key": "image_gen",
        "label": "🎨 图像生成 (Agnes AI)",
        "kind": "image",
        "placeholder": "描述你想生成的图像内容...",
    },
    {
        "key": "video_gen",
        "label": "🎬 视频生成 (Agnes AI)",
        "kind": "video",
        "placeholder": "描述你想生成的视频内容...",
    },
    {
        "key": "multimodal",
        "label": "🖼️ 多模态理解 (Agnes AI)",
        "kind": "vision",
        "placeholder": "针对上传的图片提问，例如：描述图片内容 / 图中有什么文字...",
    },
]

IMAGE_SIZE_CHOICES = ["1024x1024", "1024x768", "768x1024", "1280x720"]


class AIGCPanel(QWidget):
    """AIGC 生成面板。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.chat_worker = None
        self.image_worker = None
        self.video_worker = None
        self._raw_result = ""  # 文本类结果的原始 Markdown(复制/导出用，不是渲染后的HTML)
        self._image_bytes = None
        self._video_url = None
        self._render_timer = QTimer(self)
        self._render_timer.setInterval(100)
        self._render_timer.timeout.connect(self._flush_output_text)
        self._dirty = False
        self._init_ui()
        self._on_task_changed(0)

    def _init_ui(self):
        """初始化UI。"""
        # 外层套一个滚动区域，内容较多时可以滚动查看，不会被窗口高度裁掉
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setStyleSheet("border: none; background: transparent;")
        outer.addWidget(scroll)
        content = QWidget()
        scroll.setWidget(content)

        layout = QVBoxLayout(content)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(12)

        # ===== 标题 =====
        title = QLabel("AIGC 生成")
        title.setFont(QFont("Microsoft YaHei UI", 16, QFont.Weight.Bold))
        title.setStyleSheet("color: #cba6f7;")
        layout.addWidget(title)

        # ===== 任务类型选择 =====
        type_group = QGroupBox("任务类型")
        type_layout = QHBoxLayout(type_group)

        self.task_combo = QComboBox()
        for t in TASK_TYPES:
            self.task_combo.addItem(t["label"])
        self.task_combo.currentIndexChanged.connect(self._on_task_changed)
        self.task_combo.setStyleSheet("""
            QComboBox {
                background-color: #1e1e2e;
                border: 1px solid #313244;
                border-radius: 6px;
                padding: 8px 12px;
                color: #cdd6f4;
            }
            QComboBox::drop-down { border: none; }
            QComboBox QAbstractItemView {
                background-color: #1e1e2e;
                color: #cdd6f4;
                selection-background-color: #45475a;
            }
        """)
        type_layout.addWidget(self.task_combo)
        layout.addWidget(type_group)

        # ===== 输入区域 =====
        input_group = QGroupBox("输入提示")
        input_layout = QVBoxLayout(input_group)

        self.prompt_input = QTextEdit()
        self.prompt_input.setMaximumHeight(100)
        self.prompt_input.setStyleSheet("""
            QTextEdit {
                background-color: #1e1e2e;
                border: 1px solid #313244;
                border-radius: 8px;
                padding: 10px;
                color: #cdd6f4;
                font-size: 13px;
            }
        """)
        input_layout.addWidget(self.prompt_input)

        # 参考图片行(图像生成的可选图生图输入 / 多模态理解的必填输入)
        image_row = QHBoxLayout()
        self.image_path_edit = QLineEdit()
        self.image_path_edit.setReadOnly(True)
        self.image_path_edit.setPlaceholderText("(可选)选择一张参考图片...")
        image_row.addWidget(self.image_path_edit, 1)
        self.pick_image_btn = QPushButton("📎 选择图片")
        self.pick_image_btn.clicked.connect(self._pick_image)
        image_row.addWidget(self.pick_image_btn)
        self.clear_image_btn = QPushButton("清除")
        self.clear_image_btn.clicked.connect(self._clear_image)
        image_row.addWidget(self.clear_image_btn)
        self.image_row_widget = QWidget()
        self.image_row_widget.setLayout(image_row)
        input_layout.addWidget(self.image_row_widget)

        # 图像尺寸选择(仅图像生成任务)
        size_row = QHBoxLayout()
        size_row.addWidget(QLabel("图像尺寸:"))
        self.size_combo = QComboBox()
        self.size_combo.addItems(IMAGE_SIZE_CHOICES)
        size_row.addWidget(self.size_combo, 1)
        self.size_row_widget = QWidget()
        self.size_row_widget.setLayout(size_row)
        input_layout.addWidget(self.size_row_widget)

        layout.addWidget(input_group)

        # ===== 生成按钮 =====
        self.generate_btn = QPushButton("开始生成")
        self.generate_btn.setStyleSheet("""
            QPushButton {
                background-color: #a6e3a1;
                color: #1e1e2e;
                border-radius: 8px;
                padding: 10px 24px;
                font-weight: bold;
                font-size: 14px;
            }
            QPushButton:hover { background-color: #94e2d5; }
            QPushButton:disabled { background-color: #45475a; color: #6c7086; }
        """)
        self.generate_btn.clicked.connect(self._on_generate)
        layout.addWidget(self.generate_btn)

        # ===== 进度显示 =====
        self.progress_label = QLabel("")
        self.progress_label.setStyleSheet("color: #89b4fa; font-size: 12px; padding: 4px;")
        layout.addWidget(self.progress_label)

        # ===== 输出区域(按任务类型切换文本/图像/视频三种展示) =====
        output_group = QGroupBox("生成结果")
        output_layout = QVBoxLayout(output_group)

        self.output_stack = QStackedWidget()

        # -- 文本输出页 --
        text_page = QWidget()
        text_page_layout = QVBoxLayout(text_page)
        text_page_layout.setContentsMargins(0, 0, 0, 0)
        self.output_text = QTextEdit()
        self.output_text.setReadOnly(True)
        self.output_text.setObjectName("text-output")
        self.output_text.setStyleSheet("""
            QTextEdit {
                background-color: #1e1e2e;
                border: 1px solid #313244;
                border-radius: 8px;
                padding: 12px;
                color: #cdd6f4;
                font-family: Consolas, Courier New, monospace;
                font-size: 12px;
            }
        """)
        text_page_layout.addWidget(self.output_text)
        btn_row = QHBoxLayout()
        self.copy_btn = QPushButton("📋 复制到剪贴板")
        self.copy_btn.clicked.connect(self._copy_output)
        btn_row.addWidget(self.copy_btn)
        self.export_btn = QPushButton("💾 导出为文件")
        self.export_btn.clicked.connect(self._export_output)
        btn_row.addWidget(self.export_btn)
        text_page_layout.addLayout(btn_row)
        self.output_stack.addWidget(text_page)

        # -- 图像输出页 --
        image_page = QWidget()
        image_page_layout = QVBoxLayout(image_page)
        image_page_layout.setContentsMargins(0, 0, 0, 0)
        self.image_preview = QLabel("尚未生成图像")
        self.image_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.image_preview.setMinimumHeight(280)
        self.image_preview.setWordWrap(True)
        self.image_preview.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        self.image_preview.setStyleSheet("""
            background-color: #1e1e2e;
            border: 1px solid #313244;
            border-radius: 8px;
            color: #6c7086;
        """)
        image_page_layout.addWidget(self.image_preview, 1)
        self.save_image_btn = QPushButton("💾 保存图片")
        self.save_image_btn.clicked.connect(self._save_image)
        self.save_image_btn.setEnabled(False)
        image_page_layout.addWidget(self.save_image_btn)
        self.output_stack.addWidget(image_page)

        # -- 视频输出页 --
        video_page = QWidget()
        video_page_layout = QVBoxLayout(video_page)
        video_page_layout.setContentsMargins(0, 0, 0, 0)
        self.video_status_label = QLabel("尚未生成视频")
        self.video_status_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.video_status_label.setWordWrap(True)
        self.video_status_label.setMinimumHeight(280)
        self.video_status_label.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        self.video_status_label.setStyleSheet("""
            background-color: #1e1e2e;
            border: 1px solid #313244;
            border-radius: 8px;
            color: #6c7086;
            padding: 12px;
        """)
        video_page_layout.addWidget(self.video_status_label, 1)
        video_btn_row = QHBoxLayout()
        self.cancel_video_btn = QPushButton("⏹ 取消生成")
        self.cancel_video_btn.clicked.connect(self._cancel_video)
        self.cancel_video_btn.setEnabled(False)
        video_btn_row.addWidget(self.cancel_video_btn)
        self.open_video_btn = QPushButton("🌐 在浏览器打开")
        self.open_video_btn.clicked.connect(self._open_video)
        self.open_video_btn.setEnabled(False)
        video_btn_row.addWidget(self.open_video_btn)
        self.download_video_btn = QPushButton("⬇ 下载视频")
        self.download_video_btn.clicked.connect(self._download_video)
        self.download_video_btn.setEnabled(False)
        video_btn_row.addWidget(self.download_video_btn)
        video_page_layout.addLayout(video_btn_row)
        self.output_stack.addWidget(video_page)

        output_layout.addWidget(self.output_stack)
        layout.addWidget(output_group, 1)

    # ===== 任务类型切换 =====

    def _current_task(self) -> dict:
        return TASK_TYPES[self.task_combo.currentIndex()]

    def _on_task_changed(self, index: int):
        task = TASK_TYPES[index]
        kind = task["kind"]
        self.prompt_input.setPlaceholderText(task.get("placeholder", ""))

        self.image_row_widget.setVisible(kind in ("image", "vision"))
        self.image_path_edit.setPlaceholderText(
            "选择一张图片(必填)..." if kind == "vision" else "(可选)选择一张参考图片，用于图生图..."
        )
        self.size_row_widget.setVisible(kind == "image")

        page_index = {"chat": 0, "vision": 0, "image": 1, "video": 2}[kind]
        self.output_stack.setCurrentIndex(page_index)

    def _pick_image(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "选择图片", "", "图片文件 (*.png *.jpg *.jpeg *.webp *.bmp)"
        )
        if path:
            self.image_path_edit.setText(path)

    def _clear_image(self):
        self.image_path_edit.clear()

    # ===== 生成入口(按 kind 分派) =====

    def _on_generate(self):
        task = self._current_task()
        kind = task["kind"]
        prompt = self.prompt_input.toPlainText().strip()
        if not prompt:
            self.progress_label.setText("请输入生成提示!")
            return

        if kind == "chat":
            self._generate_chat(task, prompt)
        elif kind == "vision":
            self._generate_vision(task, prompt)
        elif kind == "image":
            self._generate_image(prompt)
        elif kind == "video":
            self._generate_video(prompt)

    def _set_generating(self, generating: bool, label: str = "生成中..."):
        self.generate_btn.setEnabled(not generating)
        self.generate_btn.setText(label if generating else "开始生成")

    # ---- chat(文本生成/代码生成/图片描述) ----

    def _generate_chat(self, task: dict, prompt: str):
        cfg = get_config()
        if not cfg.has_api_key():
            self.progress_label.setText(f"请先配置 {cfg.active_provider} 的 API Token！")
            return
        provider = registry.build_provider(cfg.active_provider, cfg)
        messages = [
            {"role": "system", "content": task["system_prompt"]},
            {"role": "user", "content": prompt},
        ]
        self._start_text_worker(provider, messages)

    # ---- vision(多模态理解，固定 Agnes AI) ----

    def _generate_vision(self, task: dict, prompt: str):
        image_path = self.image_path_edit.text().strip()
        if not image_path or not os.path.isfile(image_path):
            self.progress_label.setText("请先选择一张图片！")
            return
        cfg = get_config()
        if not cfg.has_api_key("agnes"):
            self.progress_label.setText("请先在设置中配置 Agnes AI 的 API Token！")
            return
        provider = registry.build_provider("agnes", cfg)
        content = AgnesProvider.build_vision_content(prompt, [image_path])
        messages = [{"role": "user", "content": content}]
        self._start_text_worker(provider, messages)

    def _start_text_worker(self, provider, messages: list):
        self._raw_result = ""
        self.output_text.clear()
        self.copy_btn.setEnabled(False)
        self.export_btn.setEnabled(False)
        self._set_generating(True)
        self.progress_label.setText("正在生成...")

        self.chat_worker = ChatWorker(provider, messages, stream=True)
        self.chat_worker.content_delta.connect(self._on_content_delta)
        self.chat_worker.finished_ok.connect(self._on_chat_finished)
        self.chat_worker.error_occurred.connect(self._on_chat_error)
        self.chat_worker.start()

    def _on_content_delta(self, delta: str):
        self._raw_result += delta
        self._dirty = True
        if not self._render_timer.isActive():
            self._render_timer.start()

    def _flush_output_text(self):
        if not self._dirty:
            self._render_timer.stop()
            return
        self._dirty = False
        self.output_text.setMarkdown(self._raw_result)

    def _on_chat_finished(self, content: str, reasoning: str):
        self._raw_result = content
        self._flush_output_text()
        self._render_timer.stop()
        self._set_generating(False)
        self.progress_label.setText("生成完成!")
        self.copy_btn.setEnabled(True)
        self.export_btn.setEnabled(True)

    def _on_chat_error(self, error: str):
        self._render_timer.stop()
        self._set_generating(False)
        self.progress_label.setText(f"生成失败: {error}")
        self.output_text.setHtml(f'<span style="color:#f38ba8;">❌ {error}</span>')

    def _copy_output(self):
        # 用原始 Markdown 文本，不能用 toPlainText()(会丢失代码块等标记)
        QApplication.clipboard().setText(self._raw_result)
        self.copy_btn.setText("✅ 已复制!")
        QTimer.singleShot(2000, lambda: self.copy_btn.setText("📋 复制到剪贴板"))
        self.progress_label.setText("已复制到剪贴板")

    def _export_output(self):
        if not self._raw_result.strip():
            return
        default_name = f"aigc_{self._current_task()['key']}_{datetime.now():%Y%m%d_%H%M%S}.md"
        path, _ = QFileDialog.getSaveFileName(
            self, "导出为文件", default_name, "Markdown (*.md);;文本文件 (*.txt);;所有文件 (*.*)"
        )
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self._raw_result)
            self.progress_label.setText(f"已导出到: {path}")

    # ---- image(图像生成，固定 Agnes AI) ----

    def _generate_image(self, prompt: str):
        cfg = get_config()
        if not cfg.has_api_key("agnes"):
            self.progress_label.setText("请先在设置中配置 Agnes AI 的 API Token！")
            return
        provider = registry.build_provider("agnes", cfg)
        image_path = self.image_path_edit.text().strip() or None
        if image_path and not os.path.isfile(image_path):
            image_path = None

        self._image_bytes = None
        self.save_image_btn.setEnabled(False)
        self.image_preview.setText("正在生成图像...")
        self._set_generating(True)
        self.progress_label.setText("正在生成图像...")

        self.image_worker = ImageGenWorker(
            provider, prompt, self.size_combo.currentText(), image_path
        )
        self.image_worker.image_ready.connect(self._on_image_ready)
        self.image_worker.error_occurred.connect(self._on_image_error)
        self.image_worker.start()

    def _on_image_ready(self, image_bytes: bytes):
        self._image_bytes = image_bytes
        pixmap = QPixmap()
        pixmap.loadFromData(image_bytes)
        if not pixmap.isNull():
            scaled = pixmap.scaled(
                self.image_preview.width() or 480,
                420,
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
            self.image_preview.setPixmap(scaled)
            self.save_image_btn.setEnabled(True)
            self.progress_label.setText("生成完成!")
        else:
            self.image_preview.setText("图像数据无法解析")
            self.progress_label.setText("生成失败：图像数据无法解析")
        self._set_generating(False)

    def _on_image_error(self, error: str):
        self.image_preview.setText(f"❌ {error}")
        self.progress_label.setText(f"生成失败: {error}")
        self._set_generating(False)

    def _save_image(self):
        if not self._image_bytes:
            return
        default_name = f"aigc_image_{datetime.now():%Y%m%d_%H%M%S}.png"
        path, _ = QFileDialog.getSaveFileName(
            self, "保存图片", default_name, "PNG (*.png);;JPEG (*.jpg);;所有文件 (*.*)"
        )
        if path:
            with open(path, "wb") as f:
                f.write(self._image_bytes)
            self.progress_label.setText(f"已保存到: {path}")

    # ---- video(视频生成，固定 Agnes AI) ----

    def _generate_video(self, prompt: str):
        cfg = get_config()
        if not cfg.has_api_key("agnes"):
            self.progress_label.setText("请先在设置中配置 Agnes AI 的 API Token！")
            return
        provider = registry.build_provider("agnes", cfg)

        self._video_url = None
        self.open_video_btn.setEnabled(False)
        self.download_video_btn.setEnabled(False)
        self.cancel_video_btn.setEnabled(True)
        self.video_status_label.setText("正在提交视频生成任务...")
        self._set_generating(True, "生成中(可能需要几十秒到几分钟)...")

        self.video_worker = VideoGenWorker(provider, prompt)
        self.video_worker.progress_update.connect(self.video_status_label.setText)
        self.video_worker.video_ready.connect(self._on_video_ready)
        self.video_worker.error_occurred.connect(self._on_video_error)
        self.video_worker.start()

    def _cancel_video(self):
        if self.video_worker is not None and self.video_worker.isRunning():
            self.video_worker.cancel()
        self.cancel_video_btn.setEnabled(False)
        self._set_generating(False)

    def _on_video_ready(self, video_url: str):
        self._video_url = video_url
        self.video_status_label.setText(f"视频生成完成！\n\n{video_url}")
        self.open_video_btn.setEnabled(True)
        self.download_video_btn.setEnabled(True)
        self.cancel_video_btn.setEnabled(False)
        self.progress_label.setText("生成完成!")
        self._set_generating(False)

    def _on_video_error(self, error: str):
        self.video_status_label.setText(f"❌ {error}")
        self.progress_label.setText(f"生成失败: {error}")
        self.cancel_video_btn.setEnabled(False)
        self._set_generating(False)

    def _open_video(self):
        if self._video_url:
            QDesktopServices.openUrl(QUrl(self._video_url))

    def _download_video(self):
        if not self._video_url:
            return
        default_name = f"aigc_video_{datetime.now():%Y%m%d_%H%M%S}.mp4"
        path, _ = QFileDialog.getSaveFileName(
            self, "下载视频", default_name, "MP4 (*.mp4);;所有文件 (*.*)"
        )
        if not path:
            return
        try:
            import requests

            resp = requests.get(self._video_url, timeout=120)
            resp.raise_for_status()
            with open(path, "wb") as f:
                f.write(resp.content)
            self.progress_label.setText(f"已下载到: {path}")
        except Exception as e:
            self.progress_label.setText(f"下载失败: {e}")
