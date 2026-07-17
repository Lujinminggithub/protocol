"""
首次引导/设置 - Token 配置对话框
支持首次启动引导和后续修改 Token，支持 DeepSeek 与 Agnes AI 两个 Provider。
"""

from PyQt6.QtWidgets import (
    QDialog,
    QVBoxLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QMessageBox,
    QGroupBox,
    QCheckBox,
    QComboBox,
    QTabWidget,
    QWidget,
)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QFont

from src.config import get_config
from src.providers.deepseek import DeepSeekProvider
from src.providers.agnes import AgnesProvider

_EFFORT_CHOICES = [("high", "高 (high)"), ("max", "最高 (max)")]


class ProviderKeyTab(QWidget):
    """单个 Provider(DeepSeek/Agnes AI) 的 Key/模型 配置页。"""

    def __init__(self, provider_id: str, provider_cls, help_text: str, show_model_extras: bool, parent=None):
        super().__init__(parent)
        self.provider_id = provider_id
        self.provider_cls = provider_cls
        self.show_model_extras = show_model_extras
        self._init_ui(help_text)
        self._load_current()

    def _init_ui(self, help_text: str):
        layout = QVBoxLayout(self)
        layout.setSpacing(10)

        key_row = QHBoxLayout()
        key_label = QLabel("API Key:")
        key_label.setFixedWidth(80)
        key_label.setStyleSheet("color: #a6adc8;")
        key_row.addWidget(key_label)

        self.key_input = QLineEdit()
        self.key_input.setPlaceholderText("sk-xxxxxxxxxxxxxxxx")
        self.key_input.setEchoMode(QLineEdit.EchoMode.Password)
        self.key_input.setStyleSheet("""
            QLineEdit {
                background-color: #1e1e2e;
                border: 1px solid #313244;
                border-radius: 6px;
                padding: 8px 12px;
                color: #cdd6f4;
            }
            QLineEdit:focus { border: 1px solid #cba6f7; }
        """)
        key_row.addWidget(self.key_input, 1)
        layout.addLayout(key_row)

        show_cb = QCheckBox("显示 Token")
        show_cb.setStyleSheet("color: #a6adc8;")
        show_cb.stateChanged.connect(self._toggle_echo)
        layout.addWidget(show_cb)

        if self.show_model_extras:
            model_row = QHBoxLayout()
            model_row.addWidget(QLabel("模型:"))
            self.model_combo = QComboBox()
            for value, label in self.provider_cls.MODEL_CHOICES:
                self.model_combo.addItem(label, value)
            model_row.addWidget(self.model_combo, 1)
            layout.addLayout(model_row)

            self.thinking_cb = QCheckBox("启用深度思考模式 (Thinking) — 展示AI推理过程")
            self.thinking_cb.setChecked(True)
            self.thinking_cb.setStyleSheet("color: #cdd6f4;")
            layout.addWidget(self.thinking_cb)

            effort_row = QHBoxLayout()
            effort_row.addWidget(QLabel("思考强度:"))
            self.effort_combo = QComboBox()
            for value, label in _EFFORT_CHOICES:
                self.effort_combo.addItem(label, value)
            effort_row.addWidget(self.effort_combo, 1)
            layout.addLayout(effort_row)
            self.thinking_cb.toggled.connect(self.effort_combo.setEnabled)

            hint = QLabel("提示：开启思考模式后，temperature 等参数不会生效。")
            hint.setStyleSheet("color: #6c7086; font-size: 11px;")
            hint.setWordWrap(True)
            layout.addWidget(hint)
        else:
            model_row = QHBoxLayout()
            model_row.addWidget(QLabel("模型:"))
            self.model_combo = QComboBox()
            for value, label in self.provider_cls.MODEL_CHOICES:
                self.model_combo.addItem(label, value)
            model_row.addWidget(self.model_combo, 1)
            layout.addLayout(model_row)
            self.thinking_cb = None
            self.effort_combo = None

        sep = QLabel()
        sep.setStyleSheet("border-top: 1px solid #313244; margin: 4px 0;")
        layout.addWidget(sep)

        help_label = QLabel(help_text)
        help_label.setStyleSheet("color: #6c7086; font-size: 11px; line-height: 1.6;")
        help_label.setWordWrap(True)
        layout.addWidget(help_label)
        layout.addStretch()

    def _toggle_echo(self, state):
        if state == Qt.CheckState.Checked.value:
            self.key_input.setEchoMode(QLineEdit.EchoMode.Normal)
        else:
            self.key_input.setEchoMode(QLineEdit.EchoMode.Password)

    def _load_current(self):
        cfg = get_config()
        settings = cfg.get_provider_settings(self.provider_id)
        key = settings.get("api_key", "")
        if key:
            # 无论长度多短都完整回显，避免"展示阈值"与"保存有效性阈值"不一致
            # 导致短Key不回显、用户误保存空值把已保存的Key覆盖掉的问题
            self.key_input.setText(key)
            if len(key) > 6:
                self.key_input.setPlaceholderText(f"当前已配置: sk-{key[3:6]}***{key[-3:]}")

        model = settings.get("model", "")
        idx = self.model_combo.findData(model)
        if idx >= 0:
            self.model_combo.setCurrentIndex(idx)

        if self.thinking_cb is not None:
            self.thinking_cb.setChecked(settings.get("thinking_enabled", True))
            self.effort_combo.setEnabled(self.thinking_cb.isChecked())
            effort = settings.get("reasoning_effort", "high")
            idx = self.effort_combo.findData(effort)
            if idx >= 0:
                self.effort_combo.setCurrentIndex(idx)

    def save(self):
        cfg = get_config()
        new_key = self.key_input.text().strip()
        existing_key = cfg.get_provider_settings(self.provider_id).get("api_key", "")
        kwargs = {"model": self.model_combo.currentData()}
        # 只有"填了新值"或"之前本来就没有值"时才写入 api_key，避免输入框意外为空
        # 时把已保存的 Key 静默覆盖成空字符串
        if new_key or not existing_key:
            kwargs["api_key"] = new_key
        if self.thinking_cb is not None:
            kwargs["thinking_enabled"] = self.thinking_cb.isChecked()
            kwargs["reasoning_effort"] = self.effort_combo.currentData()
        cfg.set_provider_settings(self.provider_id, **kwargs)

    def has_valid_key(self) -> bool:
        key = self.key_input.text().strip()
        return len(key) >= 10


class ToolKeyTab(QWidget):
    """轻量Key配置页 —— 给需要独立Key的工具(如 Tavily 网页搜索、和风天气)用，
    没有模型/思考强度等对话专属配置项，Key 也是可选的(留空则该工具不启用)。
    """

    def __init__(
        self,
        tool_id: str,
        display_name: str,
        help_text: str,
        parent=None,
        show_host: bool = False,
        host_placeholder: str = "",
    ):
        super().__init__(parent)
        self.tool_id = tool_id
        self.display_name = display_name
        self.show_host = show_host
        self._init_ui(help_text, host_placeholder)
        self._load_current()

    def _init_ui(self, help_text: str, host_placeholder: str):
        layout = QVBoxLayout(self)
        layout.setSpacing(10)

        self.host_input = None
        if self.show_host:
            host_row = QHBoxLayout()
            host_label = QLabel("API Host:")
            host_label.setFixedWidth(80)
            host_label.setStyleSheet("color: #a6adc8;")
            host_row.addWidget(host_label)
            self.host_input = QLineEdit()
            self.host_input.setPlaceholderText(host_placeholder or "(可选)")
            self.host_input.setStyleSheet("""
                QLineEdit {
                    background-color: #1e1e2e;
                    border: 1px solid #313244;
                    border-radius: 6px;
                    padding: 8px 12px;
                    color: #cdd6f4;
                }
                QLineEdit:focus { border: 1px solid #cba6f7; }
            """)
            host_row.addWidget(self.host_input, 1)
            layout.addLayout(host_row)

        key_row = QHBoxLayout()
        key_label = QLabel("API Key:")
        key_label.setFixedWidth(80)
        key_label.setStyleSheet("color: #a6adc8;")
        key_row.addWidget(key_label)

        self.key_input = QLineEdit()
        self.key_input.setPlaceholderText("(可选) tvly-xxxxxxxxxxxxxxxx")
        self.key_input.setEchoMode(QLineEdit.EchoMode.Password)
        self.key_input.setStyleSheet("""
            QLineEdit {
                background-color: #1e1e2e;
                border: 1px solid #313244;
                border-radius: 6px;
                padding: 8px 12px;
                color: #cdd6f4;
            }
            QLineEdit:focus { border: 1px solid #cba6f7; }
        """)
        key_row.addWidget(self.key_input, 1)
        layout.addLayout(key_row)

        show_cb = QCheckBox("显示 Token")
        show_cb.setStyleSheet("color: #a6adc8;")
        show_cb.stateChanged.connect(self._toggle_echo)
        layout.addWidget(show_cb)

        sep = QLabel()
        sep.setStyleSheet("border-top: 1px solid #313244; margin: 4px 0;")
        layout.addWidget(sep)

        help_label = QLabel(help_text)
        help_label.setStyleSheet("color: #6c7086; font-size: 11px; line-height: 1.6;")
        help_label.setWordWrap(True)
        layout.addWidget(help_label)
        layout.addStretch()

    def _toggle_echo(self, state):
        if state == Qt.CheckState.Checked.value:
            self.key_input.setEchoMode(QLineEdit.EchoMode.Normal)
        else:
            self.key_input.setEchoMode(QLineEdit.EchoMode.Password)

    def _load_current(self):
        cfg = get_config()
        settings = cfg.get_tool_settings(self.tool_id)
        key = settings.get("api_key", "")
        if key:
            self.key_input.setText(key)
            if len(key) > 6:
                self.key_input.setPlaceholderText(f"当前已配置: {key[:6]}***{key[-3:]}")
        if self.host_input is not None:
            self.host_input.setText(settings.get("api_host", ""))

    def save(self):
        cfg = get_config()
        new_key = self.key_input.text().strip()
        existing = cfg.get_tool_settings(self.tool_id).get("api_key", "")
        kwargs = {}
        # 同 ProviderKeyTab: 只有"填了新值"或"之前本来就没有值"才写入，避免误清空
        if new_key or not existing:
            kwargs["api_key"] = new_key
        if self.host_input is not None:
            kwargs["api_host"] = self.host_input.text().strip()
        if kwargs:
            cfg.set_tool_settings(self.tool_id, **kwargs)


class SetupDialog(QDialog):
    """API Token 配置对话框。

    首次启动时作为引导窗口弹出。
    后续可通过托盘菜单的"设置"项打开修改 Token。
    """

    def __init__(self, parent=None, is_setup: bool = True):
        super().__init__(parent)
        self.is_setup = is_setup
        self.setWindowTitle("AI小精灵 - 初始设置")
        self.setFixedSize(480, 560)
        self._init_ui()

    def _init_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(14)
        layout.setContentsMargins(24, 24, 24, 24)

        # ===== 标题 =====
        if self.is_setup:
            title = QLabel("欢迎使用 AI小精灵")
            subtitle = QLabel("请先配置 API Token 以启用 AI 功能")
        else:
            title = QLabel("设置 - API Token")
            subtitle = QLabel("配置 API Token 以启用 AI 功能")

        title.setFont(QFont("Microsoft YaHei UI", 18, QFont.Weight.Bold))
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        title.setStyleSheet("color: #cba6f7;")
        layout.addWidget(title)

        subtitle.setAlignment(Qt.AlignmentFlag.AlignCenter)
        subtitle.setStyleSheet("color: #a6adc8; font-size: 13px;")
        layout.addWidget(subtitle)

        sep = QLabel()
        sep.setStyleSheet("border-top: 1px solid #313244; margin: 8px 0;")
        layout.addWidget(sep)

        # ===== 对话使用哪个 Provider =====
        active_row = QHBoxLayout()
        active_label = QLabel("AI助手对话使用:")
        active_label.setStyleSheet("color: #a6adc8;")
        active_row.addWidget(active_label)
        self.active_combo = QComboBox()
        self.active_combo.addItem("DeepSeek", "deepseek")
        self.active_combo.addItem("Agnes AI", "agnes")
        active_row.addWidget(self.active_combo, 1)
        layout.addLayout(active_row)

        note = QLabel("(图像生成/视频生成/多模态理解固定使用 Agnes AI)")
        note.setStyleSheet("color: #6c7086; font-size: 11px;")
        layout.addWidget(note)

        # ===== 按 Provider 分 Tab 配置 =====
        self.tabs = QTabWidget()
        self.deepseek_tab = ProviderKeyTab(
            "deepseek",
            DeepSeekProvider,
            "如何获取 DeepSeek Token？\n"
            "1. 访问 https://platform.deepseek.com\n"
            "2. 登录后进入 API Keys 页面\n"
            "3. 创建新密钥并复制粘贴到上方",
            show_model_extras=True,
        )
        self.agnes_tab = ProviderKeyTab(
            "agnes",
            AgnesProvider,
            "如何获取 Agnes AI Token？\n"
            "1. 访问 https://agnes-ai.com 注册账号\n"
            "2. 在控制台创建 API Key\n"
            "3. 复制粘贴到上方\n"
            "Agnes AI 支持文本对话、图像生成、视频生成、多模态图文理解。",
            show_model_extras=False,
        )
        self.tabs.addTab(self.deepseek_tab, "DeepSeek")
        self.tabs.addTab(self.agnes_tab, "Agnes AI")

        self.tools_tab = ToolKeyTab(
            "tavily",
            "Tavily 网页搜索",
            "如何获取 Tavily API Key？(可选，不配置则AI助手不具备联网搜索能力)\n"
            "1. 访问 https://tavily.com 注册\n"
            "2. 控制台创建 API Key（免费额度足够日常使用）\n"
            "3. 复制粘贴到上方\n\n"
            "说明：配置后 AI 助手聊天时可自动调用网页搜索获取实时信息"
            "（新闻/股价等）。这是独立于 DeepSeek/Agnes 的第三方服务，Key 不通用。",
        )
        self.qweather_tab = ToolKeyTab(
            "qweather",
            "和风天气",
            "如何获取和风天气 API Key？(可选，不配置则天气查询使用免Key的 Open-Meteo，"
            "国内城市数据可能与实测有明显偏差)\n"
            "1. 访问 https://console.qweather.com 注册登录\n"
            "2. 控制台→设置页面，复制你账号专属的 API Host"
            "（形如 abcxyz.qweatherapi.com，账号级别唯一），"
            "前面加 https:// 填到上方\n"
            "3. 创建项目(选「免费订阅」)，进项目详情页添加凭据，"
            "认证方式选「API KEY」，创建后复制 Key 填到上方\n\n"
            "说明：配置后天气查询会优先使用和风天气(国内城市更准)，调用失败时"
            "自动回退到 Open-Meteo，不影响使用。",
            show_host=True,
            host_placeholder="(可选) https://xxxxx.qweatherapi.com",
        )
        self.tabs.addTab(self.tools_tab, "🔧 工具(可选)")
        self.tabs.addTab(self.qweather_tab, "🌤 天气(可选)")
        layout.addWidget(self.tabs, 1)

        # ===== 按钮 =====
        btn_layout = QHBoxLayout()
        btn_layout.addStretch()

        if self.is_setup:
            self.cancel_btn = QPushButton("跳过（暂不使用AI）")
            self.cancel_btn.setStyleSheet("""
                QPushButton {
                    background-color: #313244;
                    color: #a6adc8;
                    border-radius: 6px;
                    padding: 8px 16px;
                }
                QPushButton:hover { background-color: #45475a; }
            """)
            self.cancel_btn.clicked.connect(self._on_cancel)
            btn_layout.addWidget(self.cancel_btn)

        self.ok_btn = QPushButton("保存")
        self.ok_btn.setStyleSheet("""
            QPushButton {
                background-color: #cba6f7;
                color: #1e1e2e;
                border-radius: 6px;
                padding: 8px 24px;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #b4befe; }
            QPushButton:disabled { background-color: #45475a; color: #6c7086; }
        """)
        self.ok_btn.clicked.connect(self._on_save)
        btn_layout.addWidget(self.ok_btn)

        if not self.is_setup:
            self.cancel_btn = QPushButton("取消")
            self.cancel_btn.setStyleSheet("""
                QPushButton {
                    background-color: #313244;
                    color: #a6adc8;
                    border-radius: 6px;
                    padding: 8px 16px;
                }
                QPushButton:hover { background-color: #45475a; }
            """)
            self.cancel_btn.clicked.connect(self.reject)
            btn_layout.addWidget(self.cancel_btn)

        layout.addLayout(btn_layout)

        # 加载当前 active_provider
        cfg = get_config()
        idx = self.active_combo.findData(cfg.active_provider)
        if idx >= 0:
            self.active_combo.setCurrentIndex(idx)
        self.tabs.setCurrentIndex(0 if cfg.active_provider == "deepseek" else 1)

    def _on_save(self):
        active_id = self.active_combo.currentData()
        active_tab = self.deepseek_tab if active_id == "deepseek" else self.agnes_tab

        if not active_tab.has_valid_key():
            QMessageBox.warning(
                self, "提示", f"请先为「{active_tab.provider_cls.display_name}」输入有效的 API Key"
            )
            return

        # 两个 Provider 只要填写了 Key 就保存(Agnes AI 即便不是对话 Provider，
        # 图像/视频/多模态功能也需要单独配置它的 Key)
        self.deepseek_tab.save()
        self.agnes_tab.save()
        # 工具 Key 是可选项，不参与上面的有效性校验，留空也允许保存(等于不启用该工具)
        self.tools_tab.save()
        self.qweather_tab.save()

        cfg = get_config()
        cfg.active_provider = active_id

        if self.is_setup:
            QMessageBox.information(self, "成功", "API Token 配置完成！")
        else:
            QMessageBox.information(self, "成功", "API Token 已更新！")

        self.accept()

    def _on_cancel(self):
        self.reject()

    @staticmethod
    def should_show() -> bool:
        """判断是否应该显示引导对话框(两个 Provider 都未配置 Key 时才显示)。"""
        cfg = get_config()
        return not (cfg.has_api_key("deepseek") or cfg.has_api_key("agnes"))
