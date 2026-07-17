"""
AI助手面板 - 聊天界面，接入 DeepSeek API

支持流式输出、思考过程(思维链)展示、Markdown 渲染、聊天记录持久化。
"""

import time

from PyQt6.QtWidgets import (
    QVBoxLayout,
    QHBoxLayout,
    QWidget,
    QLineEdit,
    QPushButton,
    QLabel,
    QScrollArea,
    QTextBrowser,
    QToolButton,
    QFrame,
    QSizePolicy,
)
from PyQt6.QtCore import Qt, pyqtSignal, QTimer
from PyQt6.QtGui import QFont

from src.config import get_config
from src.providers import registry
from src.chat_worker import ChatWorker
from src.tools import registry as tool_registry
from src import chat_store


class AutoHeightBrowser(QTextBrowser):
    """随内容自适应宽高的只读富文本浏览器，用 Qt6 内建 Markdown 渲染，不依赖第三方库。

    气泡宽度会贴合内容(不超过 max_width)，超出则自动换行；高度按换行后的
    实际文档高度精确计算，避免出现"内容框不自适应/被截断"的问题。
    """

    def __init__(self, parent=None, max_width: int = 520):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setOpenExternalLinks(True)
        self.setFrameStyle(QFrame.Shape.NoFrame)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        # 让错误/普通文本都能被选中复制
        self.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
            | Qt.TextInteractionFlag.TextSelectableByKeyboard
            | Qt.TextInteractionFlag.LinksAccessibleByMouse
        )
        self._max_width = max_width
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)

    def _adjust_size(self):
        doc = self.document()
        # 先按最大可用宽度排版，得到不换行时的理想宽度，再据此决定真实宽度
        doc.setTextWidth(self._max_width)
        ideal = doc.idealWidth()
        width = int(min(self._max_width, ideal)) if ideal > 0 else self._max_width
        doc.setTextWidth(width)
        # +30 / +16 用于容纳样式表里的 padding(10px 14px) 与文档边距
        self.setFixedWidth(width + 30)
        self.setFixedHeight(int(doc.size().height()) + 16)

    def set_markdown_text(self, raw_text: str):
        self.setMarkdown(raw_text or "")
        self._adjust_size()

    def set_error_text(self, msg: str):
        self.setHtml(f'<span style="color:#f38ba8;">❌ {msg}</span>')
        self._adjust_size()


class ChatMessage(QWidget):
    """单条聊天消息，支持流式增量更新与可折叠思考过程。"""

    scroll_requested = pyqtSignal()

    def __init__(self, is_user: bool, parent=None):
        super().__init__(parent)
        self.is_user = is_user
        self._raw_content = ""
        self._raw_reasoning = ""
        self._dirty = False
        self._think_start = None

        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 2, 4, 2)
        layout.setSpacing(8)

        avatar = QLabel("🧑" if is_user else "🤖")
        avatar.setFixedWidth(32)
        avatar.setAlignment(Qt.AlignmentFlag.AlignCenter)
        avatar.setFont(QFont("Segoe UI Emoji", 16))

        bubble = QWidget()
        bubble_layout = QVBoxLayout(bubble)
        bubble_layout.setContentsMargins(0, 0, 0, 0)
        bubble_layout.setSpacing(4)

        self.toggle_btn = None
        self.reasoning_browser = None
        if not is_user:
            self.toggle_btn = QToolButton()
            self.toggle_btn.setCheckable(True)
            self.toggle_btn.setText("🤔 思考中...")
            self.toggle_btn.setVisible(False)
            self.toggle_btn.setStyleSheet("""
                QToolButton {
                    color: #6c7086;
                    background: transparent;
                    border: none;
                    font-size: 11px;
                    text-align: left;
                }
                QToolButton:hover { color: #a6adc8; }
            """)
            self.toggle_btn.toggled.connect(self._on_toggle)
            bubble_layout.addWidget(self.toggle_btn)

            self.reasoning_browser = AutoHeightBrowser()
            self.reasoning_browser.setStyleSheet("""
                color: #6c7086;
                font-style: italic;
                font-size: 11px;
                background-color: #181825;
                border-radius: 8px;
                padding: 6px;
            """)
            self.reasoning_browser.setVisible(False)
            bubble_layout.addWidget(self.reasoning_browser)

        # 工具调用记录区(在思考过程之后、最终答案之前，体现"思考→行动→回答"时间线)
        self.tools_toggle_btn = None
        self.tools_browser = None
        self._tool_lines = []  # [{"id","name","args","result"}]
        if not is_user:
            self.tools_toggle_btn = QToolButton()
            self.tools_toggle_btn.setCheckable(True)
            self.tools_toggle_btn.setVisible(False)
            self.tools_toggle_btn.setStyleSheet("""
                QToolButton {
                    color: #94e2d5;
                    background: transparent;
                    border: none;
                    font-size: 11px;
                    text-align: left;
                }
                QToolButton:hover { color: #a6e3a1; }
            """)
            self.tools_toggle_btn.toggled.connect(self._on_tools_toggle)
            bubble_layout.addWidget(self.tools_toggle_btn)

            self.tools_browser = AutoHeightBrowser()
            self.tools_browser.setStyleSheet("""
                color: #94e2d5;
                font-size: 11px;
                background-color: #181825;
                border-radius: 8px;
                padding: 6px;
            """)
            self.tools_browser.setVisible(False)
            bubble_layout.addWidget(self.tools_browser)

        self.content_browser = AutoHeightBrowser()
        if is_user:
            self.content_browser.setStyleSheet("""
                background-color: #45475a;
                border: 1px solid #585b70;
                border-radius: 12px;
                padding: 10px 14px;
            """)
        else:
            self.content_browser.setStyleSheet("""
                background-color: #313244;
                border-radius: 12px;
                padding: 10px 14px;
            """)
        bubble_layout.addWidget(self.content_browser)

        if is_user:
            layout.addStretch(1)
            layout.addWidget(bubble, 0, Qt.AlignmentFlag.AlignTop)
            layout.addWidget(avatar, 0, Qt.AlignmentFlag.AlignTop)
        else:
            layout.addWidget(avatar, 0, Qt.AlignmentFlag.AlignTop)
            layout.addWidget(bubble, 0, Qt.AlignmentFlag.AlignTop)
            layout.addStretch(1)

        self._render_timer = QTimer(self)
        self._render_timer.setInterval(100)  # 100ms 节流重渲染，避免逐token跑Markdown解析
        self._render_timer.timeout.connect(self._flush)

    # ===== 流式增量接口 =====

    def append_reasoning(self, delta: str):
        if self._think_start is None:
            self._think_start = time.monotonic()
        self._raw_reasoning += delta
        self._dirty = True
        if self.toggle_btn is not None:
            self.toggle_btn.setVisible(True)
            self.toggle_btn.setChecked(True)  # 思考阶段默认展开
            self.toggle_btn.setText("🤔 思考中...")
        if self.reasoning_browser is not None:
            self.reasoning_browser.setVisible(True)
        if not self._render_timer.isActive():
            self._render_timer.start()

    def append_content(self, delta: str):
        if self._raw_reasoning and self.toggle_btn is not None and self.toggle_btn.isChecked():
            self._collapse_reasoning()
        self._raw_content += delta
        self._dirty = True
        if not self._render_timer.isActive():
            self._render_timer.start()

    def _collapse_reasoning(self):
        elapsed = time.monotonic() - self._think_start if self._think_start else 0
        self.toggle_btn.setChecked(False)
        self.toggle_btn.setText(f"🤔 已思考 {elapsed:.1f}s ▸ 点击查看")

    def _on_toggle(self, checked):
        if self.reasoning_browser is not None:
            self.reasoning_browser.setVisible(checked)

    def _on_tools_toggle(self, checked):
        if self.tools_browser is not None:
            self.tools_browser.setVisible(checked)

    # ===== 工具调用增量接口 =====

    def add_tool_call_start(self, call_id: str, name: str, args_display: str):
        """工具调用开始时的中间态展示(离散事件，不接入token级渲染节流，直接同步渲染)。"""
        self._tool_lines.append({"id": call_id, "name": name, "args": args_display, "result": None})
        self.tools_toggle_btn.setVisible(True)
        self.tools_toggle_btn.setChecked(True)  # 调用中默认展开，同思考过程的展开策略
        self.tools_toggle_btn.setText("🔧 正在调用工具...")
        self.tools_browser.setVisible(True)
        self._render_tool_lines()
        self.scroll_requested.emit()

    def add_tool_call_finish(self, call_id: str, name: str, args_display: str, result_display: str):
        for line in self._tool_lines:
            if line["id"] == call_id:
                line["result"] = result_display
                break
        self._render_tool_lines()
        if self._tool_lines and all(l["result"] is not None for l in self._tool_lines):
            self.tools_toggle_btn.setChecked(False)
            self.tools_toggle_btn.setText(f"🔧 已调用 {len(self._tool_lines)} 个工具 ▸ 点击查看")
        self.scroll_requested.emit()

    def _render_tool_lines(self):
        rows = []
        for line in self._tool_lines:
            mark = "⏳" if line["result"] is None else "🔧"
            tail = "调用中..." if line["result"] is None else f"→ {line['result']}"
            rows.append(f"{mark} `{line['name']}({line['args']})` {tail}")
        self.tools_browser.set_markdown_text("\n\n".join(rows))

    def _flush(self):
        if not self._dirty:
            self._render_timer.stop()
            return
        self._dirty = False
        if self._raw_reasoning and self.reasoning_browser is not None:
            self.reasoning_browser.set_markdown_text(self._raw_reasoning)
        if self._raw_content:
            self.content_browser.set_markdown_text(self._raw_content)
        self.scroll_requested.emit()

    def set_final(self, content: str, reasoning: str = ""):
        """直接设置最终内容(用于非流式完成兜底、或从历史回放)。"""
        self._raw_content = content
        self._raw_reasoning = reasoning
        self._dirty = True  # 必须置脏，否则 _flush() 会因 _dirty=False 直接跳过渲染，显示空白
        self._flush()
        self._render_timer.stop()
        if not reasoning and self.toggle_btn is not None:
            self.toggle_btn.setVisible(False)

    def set_error(self, msg: str):
        self._render_timer.stop()
        self.content_browser.set_error_text(msg)


class AIPanel(QWidget):
    """AI助手面板，对接 DeepSeek API。"""

    # AI 通过工具调用修改了日历数据时发出，供 window.py 接到 CalendarPanel.reload_from_disk
    calendar_modified = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("chat-area")
        self.scroll_area = None
        self.worker = None
        self.chat_history = chat_store.load_history()  # 启动时恢复历史(只含content)
        self._init_ui()
        self._replay_history()

    def _init_ui(self):
        """初始化UI。"""
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        # ===== 顶部工具条 =====
        toolbar = QHBoxLayout()
        toolbar.setContentsMargins(12, 8, 12, 0)
        toolbar.addStretch()
        clear_btn = QPushButton("🗑 清空对话")
        clear_btn.setStyleSheet("""
            QPushButton {
                background-color: #313244;
                color: #a6adc8;
                border-radius: 6px;
                padding: 4px 10px;
                font-size: 11px;
            }
            QPushButton:hover { background-color: #45475a; color: #cdd6f4; }
        """)
        clear_btn.clicked.connect(self.clear_chat)
        toolbar.addWidget(clear_btn)
        layout.addLayout(toolbar)

        # ===== 聊天消息区域 =====
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setStyleSheet("border: none; background-color: transparent;")
        self.scroll_area = scroll

        scroll_content = QWidget()
        self.chat_layout = QVBoxLayout(scroll_content)
        self.chat_layout.setAlignment(Qt.AlignmentFlag.AlignTop)
        self.chat_layout.setSpacing(8)
        self.chat_layout.setContentsMargins(12, 12, 12, 12)

        scroll.setWidget(scroll_content)
        layout.addWidget(scroll, 1)

        # ===== 输入区域 =====
        input_bar = QWidget()
        input_bar.setObjectName("input-bar")
        input_bar_layout = QHBoxLayout(input_bar)
        input_bar_layout.setContentsMargins(12, 8, 12, 8)
        input_bar_layout.setSpacing(8)

        self.input_field = QLineEdit()
        self.input_field.setPlaceholderText("输入消息... (按 Enter 发送)")
        self.input_field.setObjectName("input-field")
        self.input_field.returnPressed.connect(self._on_send)
        input_bar_layout.addWidget(self.input_field, 1)

        self.send_btn = QPushButton("发送")
        self.send_btn.setObjectName("send-btn")
        self.send_btn.clicked.connect(self._on_send_or_stop)
        input_bar_layout.addWidget(self.send_btn)

        layout.addWidget(input_bar)

    def _replay_history(self):
        """从持久化历史回放聊天气泡(不含 reasoning，因为历史里从不保存它)。"""
        for turn in self.chat_history:
            msg = ChatMessage(is_user=(turn.get("role") == "user"))
            msg.scroll_requested.connect(self._scroll_to_bottom)
            self.chat_layout.addWidget(msg)
            msg.set_final(turn.get("content", ""))
        self._scroll_to_bottom()

    def _set_generating(self, generating: bool):
        self.send_btn.setText("停止" if generating else "发送")
        self.input_field.setEnabled(not generating)

    def _on_send_or_stop(self):
        if self.worker is not None and self.worker.isRunning():
            self.worker.cancel()
            return
        self._on_send()

    def _on_send(self):
        """处理发送消息。"""
        text = self.input_field.text().strip()
        if not text:
            return

        self.input_field.clear()

        # 显示用户消息
        user_msg = ChatMessage(is_user=True)
        user_msg.scroll_requested.connect(self._scroll_to_bottom)
        self.chat_layout.addWidget(user_msg)
        user_msg.set_final(text)
        self._scroll_to_bottom()

        self.chat_history.append({"role": "user", "content": text})

        # 检查是否配置了 API
        cfg = get_config()
        if not cfg.has_api_key():
            err_msg = ChatMessage(is_user=False)
            self.chat_layout.addWidget(err_msg)
            err_msg.set_final(
                f"请先配置 {cfg.active_provider} 的 API Token 才能使用 AI 功能。\n"
                "点击托盘图标 → 设置 → 配置 Token。"
            )
            self._scroll_to_bottom()
            return

        # 每次发送时按当前配置现建 Provider —— 修复"更新Key后不生效"的问题
        provider = registry.build_provider(cfg.active_provider, cfg)
        tools_schema = tool_registry.get_tools_schema(cfg)

        assistant_msg = ChatMessage(is_user=False)
        assistant_msg.scroll_requested.connect(self._scroll_to_bottom)
        self.chat_layout.addWidget(assistant_msg)
        self._scroll_to_bottom()

        system_msg = {"role": "system", "content": provider.SYSTEM_PROMPT}
        messages = [system_msg] + self.chat_history[-20:]  # 保留最近20条上下文

        self._set_generating(True)
        self.worker = ChatWorker(provider, messages, stream=True, tools=tools_schema)
        self.worker.reasoning_delta.connect(assistant_msg.append_reasoning)
        self.worker.content_delta.connect(assistant_msg.append_content)
        self.worker.tool_call_started.connect(assistant_msg.add_tool_call_start)
        self.worker.tool_call_finished.connect(
            lambda cid, name, args, res: self._on_tool_call_finished(
                cid, name, args, res, assistant_msg
            )
        )
        self.worker.finished_ok.connect(
            lambda c, r: self._on_finished(c, r, assistant_msg)
        )
        self.worker.error_occurred.connect(
            lambda e: self._on_error(e, assistant_msg)
        )
        self.worker.start()

    def _on_tool_call_finished(self, call_id, name, args_display, result_display, msg: ChatMessage):
        msg.add_tool_call_finish(call_id, name, args_display, result_display)
        if name == "add_calendar_event":
            self.calendar_modified.emit()

    def _on_finished(self, content: str, reasoning: str, msg: ChatMessage):
        """收到 API 回复。"""
        msg.set_final(content, reasoning)
        if content:
            # 多轮历史只存 content，绝不写入 reasoning_content
            self.chat_history.append({"role": "assistant", "content": content})
            chat_store.save_history(self.chat_history)
        self._set_generating(False)
        self._scroll_to_bottom()

    def _on_error(self, error: str, msg: ChatMessage):
        """API 调用出错。"""
        msg.set_error(error)
        self._set_generating(False)
        self._scroll_to_bottom()

    def _scroll_to_bottom(self):
        """滚动到最底部。"""
        if self.scroll_area:
            vbar = self.scroll_area.verticalScrollBar()
            if vbar:
                vbar.setValue(vbar.maximum())

    def clear_chat(self):
        """清空聊天记录(界面 + 持久化)。"""
        while self.chat_layout.count():
            child = self.chat_layout.takeAt(0)
            if child.widget():
                child.widget().deleteLater()
        self.chat_history.clear()
        chat_store.clear_history()
